#!/usr/bin/env python3
"""Sample-exact check of a streamed SPU ring against an OPENPETE_AUDIO_DUMP.

A game that streams dialogue through SPU RAM keys one looping voice over a
ring of chunks and refills the halves with SpuWrite. Its intended output is
fully determined by the chunks it wrote (OPENPETE_SPU_WRITE_DUMP) and the
voice's pitch: this tool decodes the chunks with the PSX ADPCM filters,
resamples them to 44.1 kHz at the pitch, and cross-correlates each chunk's
span of the mixed dump against it. Every chunk must correlate (a wrong or
stale chunk does not), every lag must equal the first and the lag's slope
over the chunks must be zero (a resync that dropped or repeated samples
shifts it), and every expected chunk must carry signal (a silent
expectation correlates with anything). The time axis is the engine's audio
clock, 735 frames per VBL. The expectation decodes each chunk with the
predictor reset, where the SPU carries it across the chunk boundary: the
first samples of every chunk differ slightly, a fraction of a percent of
correlation on a 1575-block chunk. The lag slope threshold is 0.5 samples
per chunk.

usage: audio_ring_check.py --dump DUMP.raw --writes DIR --sample0-vbl V0
                           --key-vbl K --ring LO,HI --pitch P
                           [--chunks N] [--min-corr 0.8] [--max-slip 2]

--ring names the two half addresses (hex); writes at either address at or
after the key-on VBL are the ring's chunks in order. --sample0-vbl is the
VBL of the dump's first sample (the "sample 0 is vbl" log line).
"""
import argparse, glob, os, sys
import numpy as np

SR = 44100
FILTERS = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]


def decode_adpcm(data):
    data = bytes(data)
    out = np.zeros((len(data) // 16) * 28, dtype=np.float64)
    p1 = p2 = 0.0
    k = 0
    for b in range(0, len(data) - 15, 16):
        shift = data[b] & 0x0F
        f = (data[b] >> 4) & 0x0F
        f0, f1 = FILTERS[min(f, 4)]
        for i in range(2, 16):
            byte = data[b + i]
            for nib in (byte & 0x0F, byte >> 4):
                v = nib << 12
                if v & 0x8000:
                    v -= 0x10000
                v = (v >> shift) if shift < 16 else 0
                s = v + (p1 * f0 + p2 * f1) / 64.0
                s = max(-32768.0, min(32767.0, s))
                out[k] = s
                k += 1
                p2, p1 = p1, s
    return out / 32768.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", required=True)
    ap.add_argument("--writes", required=True)
    ap.add_argument("--sample0-vbl", type=float, required=True)
    ap.add_argument("--key-vbl", type=int, required=True)
    ap.add_argument("--ring", required=True)
    ap.add_argument("--pitch", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--chunks", type=int, default=0)
    ap.add_argument("--min-corr", type=float, default=0.8)
    ap.add_argument("--max-slip", type=int, default=2)
    ap.add_argument("--min-rms", type=float, default=0.005)
    ap.add_argument("--min-chunks", type=int, default=2)
    a = ap.parse_args()
    halves = [int(x, 16) for x in a.ring.split(",")]
    writes = []
    for path in sorted(glob.glob(os.path.join(a.writes, "*.bin"))):
        vbl, addr = os.path.basename(path)[:-4].split("_")
        vbl, addr = int(vbl), int(addr, 16)
        if addr in halves and vbl >= a.key_vbl:
            writes.append((vbl, addr, path))
    if a.chunks:
        writes = writes[: a.chunks]
    if len(writes) < a.min_chunks:
        print(f"FAIL: {len(writes)} ring write(s) found, {a.min_chunks} needed to test a re-sync"); sys.exit(2)
    pcm = np.fromfile(a.dump, dtype="<i2")
    pcm = pcm[: len(pcm) - len(pcm) % 2].reshape(-1, 2).astype(np.float64) / 32768
    mono = pcm.mean(axis=1)
    ratio = 4096.0 / a.pitch  # output samples per decoder sample
    start = (a.key_vbl - a.sample0_vbl) * 735.0
    fpos = start  # chunk start in output samples, carried as a float
    lags = []
    ok = True
    print(f"chunks={len(writes)} key sample={start:.1f} ratio={ratio:.4f}")
    for i, (vbl, addr, path) in enumerate(writes):
        dec = decode_adpcm(np.fromfile(path, dtype=np.uint8))
        pos = int(round(fpos))
        n_out = int(len(dec) * ratio)
        exp = np.interp(np.arange(n_out) / ratio, np.arange(len(dec)), dec)
        margin = 2048
        lo = max(0, pos - margin)
        seg = mono[lo : pos + n_out + margin]
        if len(seg) < n_out:
            print(f"chunk {i} vbl {vbl} addr {addr:#x}: dump ends inside it ({len(seg)} < {n_out})"); ok = False; break
        # normalized cross-correlation over lags within the margin (FFT)
        e = exp - exp.mean(); en = np.sqrt((e * e).sum()) + 1e-12
        n = 1 << (len(seg) + n_out).bit_length()
        corr = np.fft.irfft(np.fft.rfft(seg, n) * np.conj(np.fft.rfft(e, n)), n)[: len(seg) - n_out + 1]
        sq = np.concatenate([[0.0], np.cumsum(seg * seg)])
        energy = np.sqrt(sq[n_out:] - sq[: len(seg) - n_out + 1]) + 1e-12
        c = corr / (energy * en)
        j = int(np.argmax(c))
        best, best_lag = float(c[j]), j - (pos - lo)
        lags.append(best_lag)
        exp_rms = float(np.sqrt(np.mean(exp * exp)))
        got = seg[j : j + n_out]
        got_rms = float(np.sqrt(np.mean(got * got)))
        status = "ok"
        if exp_rms < a.min_rms:
            status = "FAIL (expected chunk is silent)"
        elif best < a.min_corr or abs(best_lag - lags[0]) > a.max_slip:
            status = "FAIL"
        if status != "ok":
            ok = False
        print(f"chunk {i} vbl {vbl} addr {addr:#x} samples {n_out}: corr {best:.3f} lag {best_lag:+d} "
              f"rms exp {exp_rms:.4f} got {got_rms:.4f} {status}")
        fpos += len(dec) * ratio
    if len(lags) >= 2:
        x = np.arange(len(lags))
        slope = float(np.polyfit(x, np.array(lags, dtype=float), 1)[0])
        print(f"lag slope {slope:+.3f} samples/chunk")
        if abs(slope) > 0.5:
            ok = False
            print("FAIL: lag drifts across chunks")
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 2)


if __name__ == "__main__":
    main()
