#!/usr/bin/env python3
"""Structural check of a streamed SPU ring from an OPENPETE_AUDIO_HEAD_LOG.

A game that streams speech through SPU RAM keys one looping voice over a
ring of two halves and refills a half with SpuWrite while the voice plays
the other. The head log (games/common/psyq/psyq_thunks.cpp) holds a `k`
line per voice keyed on or off and, before every SpuWrite, a `w` line per
running voice whose decode window the write overlaps: the guest head
(`dec`, decoder samples since key-on; `hb`/`ib`, the block it is in) and
the OpenAL source offset (`al_so`, buffer samples from the window start
`win`, one per decoder sample).

A run is one voice from key-on to its first key-off. A run is a ring run
when at least two writes of one length H land at the voice's start
address or at start + H. The half holds dh = H / 16 * 28 decoder samples;
chunk c plays from decoder sample c * dh, in half c % 2. The first ring
write after key-on is chunk 1 when it lands in the second half (chunk 0
was written before key-on), else chunk 2; each later write is the next
chunk, and a write in the wrong half is reported. Per write of chunk c,
from the guest head dec (exact, decoder samples):

  overwrite = min(dh, max(0, (c - 1) * dh - dec))
      the unplayed tail of chunk c - 2 the write replaces (a splice);
  stale     = min(dh, max(0, dec - c * dh))
      the samples of the half the head already played under the old
      content (a late refill);
  lead      = dec - (c - 1) * dh, in output ms
      the time since the head left the half (negative: overwrite);
  slip      = OpenAL ring position - guest ring position, decoder samples,
      wrapped to [-dh, dh); a slip step is a change of more than 2 between
      consecutive writes of the run (each reading floors a fractional
      position and a re-sync drops one, so a step within 2 is no slip;
      an OpenAL mixer update is 1024 output samples).

Output samples per decoder sample are 4096 / pitch. The lead is fitted as
a + b * j over j = c - 2 (the chunk index the write would overwrite).

Body check (with --dump and --writes): each ring chunk's payload
(OPENPETE_SPU_WRITE_DUMP file <vbl>_<addr>.bin) is decoded with the PSX
ADPCM filters (predictor reset per chunk), resampled at the pitch and
cross-correlated against the mixed OPENPETE_AUDIO_DUMP at its expected
position: the key-on's capture frame (`out`) plus c * dh * 4096 / pitch,
mapped to a file frame through DUMP.idx (`vbl file_frames produced`
per VBL, the dump's recorded rate). A chunk cut short by the key-off is
correlated over its played part; one with under 100 ms played, or whose
expectation is silent (RMS below --min-rms, where any signal correlates),
is listed and not judged. This is self-consistency: the
expectation is the engine's own payload, not a hardware reference.

Reference (with --ref TRACE, a DuckStation capture with the spuwrite
.spuq kind, the watched stream word and its mix WAV; the first ring run
only): the same overwrite, stale and lead are read from the reference's
voice head at each ring SpuWrite (ring position only: the start, current
block and pitch counter; a head in the written half is a tail overwrite
in its second part and a stale replay in its first), the chunk from the
watched word's last value, beside openpete's lead. The body check then
correlates openpete's mix over each chunk against the reference mix
(WAV frame = CPU tick / 768) instead of the self-consistency check.

Exit 0 PASS, 2 FAIL (any overwrite, stale, slip step, wrong-half write,
body correlation below --min-corr, a lag that moves, a run without a
key-off, or zero ring writes).

usage: audio_ring_check.py --head-log LOG [--dump DUMP.raw --writes DIR]
                           [--min-corr 0.95] [--max-lag-move 2]
                           [--expect-runs N] [--voice V] [--ref TRACE]
"""
import argparse, os, re, sys
import numpy as np

SR = 44100
FILTERS = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]
AL_PLAYING = 0x1012
SLIP_TOL = 2
KV = re.compile(r"(\w+)=(\S+)")


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


def num(x):
    return int(x, 0)


def parse(path):
    recs = []
    for line in open(path):
        if not line or line[0] not in "wk":
            continue
        d = dict(KV.findall(line))
        d["kind"] = line[0]
        for key in ("vbl", "v", "on", "dec", "al_so", "out", "pitch", "head", "ib", "kp"):
            if key in d:
                d[key] = int(d[key], 0)
        for key in ("lo", "hi", "addr", "loop", "hb", "al_state"):
            if key in d:
                d[key] = int(d[key], 16)
        if "win" in d:
            a, b = d["win"].split("-")
            d["win_lo"], d["win_hi"] = int(a, 16), int(b, 16)
        recs.append(d)
    return recs


def runs_of(recs):
    """Split per-voice key-on .. first key-off spans; writes attach to the open run."""
    open_runs, done = {}, []
    for r in recs:
        v = r["v"]
        if r["kind"] == "k":
            if r["on"] == 1:
                if v in open_runs:
                    done.append(open_runs.pop(v))
                open_runs[v] = {"v": v, "on": r, "off": None, "writes": []}
            elif v in open_runs:
                run = open_runs.pop(v)
                run["off"] = r
                done.append(run)
        elif v in open_runs:
            open_runs[v]["writes"].append(r)
    done.extend(open_runs.values())
    done.sort(key=lambda x: x["on"]["vbl"])
    return done


def ring_of(run):
    addr = run["on"]["addr"]
    lens = {}
    for w in run["writes"]:
        n = w["hi"] - w["lo"]
        if w["lo"] in (addr, addr + n):
            lens[n] = lens.get(n, 0) + 1
    if not lens:
        return None
    n, cnt = max(lens.items(), key=lambda kv: kv[1])
    return n if cnt >= 2 else None


class Dump:
    def __init__(self, raw):
        pcm = np.fromfile(raw, dtype="<i2")
        pcm = pcm[: len(pcm) - len(pcm) % 2].reshape(-1, 2).astype(np.float64) / 32768
        self.mono = pcm.mean(axis=1)
        idx = np.loadtxt(raw + ".idx", comments="#", dtype=np.int64, ndmin=2)
        self.idx = idx
        off = idx[:, 2] - idx[:, 1]
        self.offset = int(off[-1])
        self.offset_moves = int(np.count_nonzero(np.diff(off)))

    def rate(self, v0, v1):
        sel = (self.idx[:, 0] >= v0) & (self.idx[:, 0] <= v1)
        s = self.idx[sel]
        if len(s) < 2:
            return float("nan")
        return float(s[-1, 2] - s[0, 2]) / float(s[-1, 0] - s[0, 0])


def correlate(mono, exp, pos, margin=2048):
    n_out = len(exp)
    lo = max(0, pos - margin)
    seg = mono[lo: pos + n_out + margin]
    if len(seg) < n_out:
        return None, None, None
    e = exp - exp.mean()
    en = np.sqrt((e * e).sum()) + 1e-12
    n = 1 << (len(seg) + n_out).bit_length()
    corr = np.fft.irfft(np.fft.rfft(seg, n) * np.conj(np.fft.rfft(e, n)), n)[: len(seg) - n_out + 1]
    sq = np.concatenate([[0.0], np.cumsum(seg * seg)])
    energy = np.sqrt(sq[n_out:] - sq[: len(seg) - n_out + 1]) + 1e-12
    c = corr / (energy * en)
    j = int(np.argmax(c))
    return float(c[j]), j - (pos - lo), float(np.sqrt(np.mean(exp * exp)))


class Ref:
    """A DuckStation reference capture (scripts/capture_psx_oracle_ds.sh with
    the spuwrite .spuq kind, the watched stream word and --spu-dump)."""

    def __init__(self, trace):
        self.trace = trace
        self.watch = []
        if os.path.exists(trace + ".watch"):
            for line in open(trace + ".watch"):
                f = line.split()
                if len(f) < 7 or not f[0].isdigit():
                    continue
                self.watch.append((int(f[6]), int(f[5], 16)))
        self.keys, self.writes = [], []
        for line in open(trace + ".spuq"):
            f = line.split()
            if len(f) < 8 or not f[0].isdigit():
                continue
            if f[3] == "setkey":
                self.keys.append((int(f[6]), int(f[7]), int(f[8], 16)))
            elif f[3] == "spuwrite" and len(f) >= 10:
                heads = [tuple(int(x, 0) for x in t.split(":")) for t in f[10:]]
                self.writes.append((int(f[2]), int(f[6]), int(f[8]), heads))
        self.wav = None
        if os.path.exists(trace + ".wav"):
            import wave
            w = wave.open(trace + ".wav")
            pcm = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
            self.wav = pcm.reshape(-1, w.getnchannels()).astype(np.float64).mean(axis=1) / 32768

    def ring(self, addr, H):
        """The reference's ring writes of length H with a running voice at addr:
        (vbl, tick, chunk, ring position in decoder samples), and its key-on tick."""
        rows = []
        for vbl, tick, size, heads in self.writes:
            hs = [h for h in heads if h[1] == addr]
            if size != H or not hs:
                continue
            v, start, cur, counter = hs[0]
            pos = (cur - start) // 16 * 28 + (counter >> 12)
            chunk = None
            prior = [w for w in self.watch if w[0] <= tick]
            if prior and prior[-1][1] < 0x80000000 and tick - prior[-1][0] < 4000:
                chunk = prior[-1][1]
            rows.append([vbl, tick, chunk, pos, v])
        if not rows:
            return [], None
        for k, r in enumerate(rows):
            if r[2] is None:
                r[2] = (rows[0][2] or 1) + k
        v = rows[0][4]
        ons = [t for t, on, m in self.keys if on == 1 and m & (1 << v) and t < rows[0][1]]
        return rows, (ons[-1] if ons else None)


def half_reading(pos, c, dh):
    """Overwrite, stale and lead (decoder samples) of a write of chunk c from a ring
    position alone: the head inside the written half is a tail overwrite in its
    second part and a stale replay in its first."""
    rel = (pos - (c % 2) * dh) % (2 * dh)
    if rel >= dh:
        return 0, 0, rel - dh
    if rel >= dh // 2:
        return dh - rel, 0, rel - dh
    return 0, rel, -rel


def payload(writes_dir, vbl, addr):
    p = os.path.join(writes_dir, f"{vbl:08d}_{addr:06x}.bin")
    return p if os.path.exists(p) else None


def preload_chunks(writes_dir, addr, H, key_vbl, c0):
    """Payload files of the chunks written before key-on (chunk 0, and chunk 1 when c0 == 2)."""
    out = {}
    names = sorted(os.listdir(writes_dir)) if writes_dir else []
    for c, a in ((0, addr), (1, addr + H)):
        if c >= c0:
            continue
        best = None
        for nm in names:
            if not nm.endswith(".bin"):
                continue
            v, ad = nm[:-4].split("_")
            if int(ad, 16) == a and int(v) <= key_vbl and os.path.getsize(os.path.join(writes_dir, nm)) == H:
                best = os.path.join(writes_dir, nm)
        if best:
            out[c] = best
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--head-log", required=True)
    ap.add_argument("--dump")
    ap.add_argument("--writes")
    ap.add_argument("--min-corr", type=float, default=0.95)
    ap.add_argument("--max-lag-move", type=int, default=2)
    ap.add_argument("--min-rms", type=float, default=0.005)
    ap.add_argument("--expect-runs", type=int, default=0)
    ap.add_argument("--voice", type=int, default=-1)
    ap.add_argument("--tag", default="")
    ap.add_argument("--ref", help="DuckStation reference capture (the .bin; reads .spuq, .watch, .wav)")
    a = ap.parse_args()

    recs = parse(a.head_log)
    dump = Dump(a.dump) if a.dump and a.writes else None
    ref = Ref(a.ref) if a.ref else None
    rings = []
    for run in runs_of(recs):
        if a.voice >= 0 and run["v"] != a.voice:
            continue
        H = ring_of(run)
        if H:
            rings.append((run, H))
    fails = []
    tot = dict(writes=0, over_n=0, over_s=0.0, over_max=0.0, stale_n=0, stale_s=0.0,
               slip_steps=0, wrong_half=0, walk_bad=0, no_al=0, body_n=0, body_bad=0)
    min_corr = None
    tag = a.tag or os.path.basename(a.head_log)
    if dump:
        print(f"dump: file frame = capture frame - {dump.offset}; offset moves {dump.offset_moves}")
        if dump.offset_moves:
            fails.append("capture ring dropped frames: the dump is not a timeline")
    if not rings:
        fails.append("no ring run (engagement 0)")
    if a.expect_runs and len(rings) != a.expect_runs:
        fails.append(f"{len(rings)} ring runs, {a.expect_runs} expected")
    for r, (run, H) in enumerate(rings):
        on, off = run["on"], run["off"]
        addr, v = on["addr"], run["v"]
        dh = H // 16 * 28
        ring = 2 * dh
        writes = [w for w in run["writes"] if w["hi"] - w["lo"] == H and w["lo"] in (addr, addr + H)]
        c0 = 1 if writes[0]["lo"] == addr + H else 2
        pitch = on["pitch"] or writes[0]["pitch"]
        o_per_d = 4096.0 / pitch
        to_ms = o_per_d / SR * 1000.0
        if off is None:
            fails.append(f"run {r} (v{v}, key-on vbl {on['vbl']}) has no key-off: the row stopped first")
        span = f"vbl {on['vbl']}-{off['vbl'] if off else '?'}"
        rate = ""
        if dump and off:
            rate = f" dump rate {dump.rate(on['vbl'], off['vbl']):.3f} frames/VBL"
        print(f"run {r}: v{v} addr {addr:#x} half {H:#x} ({dh} dec samples, {dh * o_per_d:.1f} out) "
              f"pitch {pitch} {span} ring writes {len(writes)} first chunk {c0}{rate}")
        print("  chunk   vbl   half  lead_ms  over_dec  over_out stale_dec  slip  step  corr    lag  status")
        prev_slip = None
        js, leads = [], []
        lag0 = None
        bodies = {}
        if dump:
            bodies.update(preload_chunks(a.writes, addr, H, on["vbl"], c0))
        for k, w in enumerate(writes):
            c = c0 + k
            if dump:
                p = payload(a.writes, w["vbl"], w["lo"])
                if p:
                    bodies[c] = p
            st = []
            if c % 2 != (0 if w["lo"] == addr else 1):
                tot["wrong_half"] += 1
                st.append("WRONG-HALF")
            dec = w["dec"]
            over = min(dh, max(0, (c - 1) * dh - dec))
            stale = min(dh, max(0, dec - c * dh))
            lead = (dec - (c - 1) * dh) * to_ms
            js.append(c - 2)
            leads.append(lead)
            exp_hb = addr + ((dec % ring) // 28) * 16
            if w.get("head") != 1 or w.get("hb") != exp_hb or w.get("ib") != dec % 28:
                tot["walk_bad"] += 1
                st.append("WALK")
            slip, step = None, ""
            if w.get("al_state") == AL_PLAYING and w.get("al_so", -1) >= 0:
                al_pos = ((w["win_lo"] - addr) // 16 * 28 + w["al_so"]) % ring
                slip = (al_pos - dec % ring + dh) % ring - dh
                if prev_slip is not None:
                    d = slip - prev_slip
                    step = f"{d:+d}"
                    if abs(d) > SLIP_TOL:
                        tot["slip_steps"] += 1
                        st.append("SLIP")
                prev_slip = slip
            else:
                tot["no_al"] += 1
            if over:
                tot["over_n"] += 1
                tot["over_s"] += over * to_ms
                tot["over_max"] = max(tot["over_max"], over * to_ms)
                st.append("OVERWRITE")
            if stale:
                tot["stale_n"] += 1
                tot["stale_s"] += stale * to_ms
                st.append("STALE")
            tot["writes"] += 1
            w["_row"] = (c, w["vbl"], w["lo"], lead, over, over * o_per_d, stale,
                         "-" if slip is None else f"{slip:+d}", step or "-", st)
        # body correlation per chunk, including the chunks written before key-on
        corr_of = {}
        seg_of = {}
        use_ref = ref is not None and r == 0
        if dump:
            key_file = on["out"] + 1 - dump.offset
            end_file = (off["out"] - dump.offset) if off else len(dump.mono)
            for c in sorted(bodies):
                dec_s = decode_adpcm(np.fromfile(bodies[c], dtype=np.uint8))
                start = key_file + c * dh * o_per_d
                pos = int(round(start))
                n_out = int(len(dec_s) * o_per_d)
                n_out = min(n_out, end_file - pos)
                if n_out < SR // 10:
                    corr_of[c] = ("-", "-", "cut")
                    continue
                exp = np.interp(np.arange(n_out) / o_per_d, np.arange(len(dec_s)), dec_s)
                cc, lag, rms = correlate(dump.mono, exp, pos)
                if cc is None:
                    corr_of[c] = ("-", "-", "DUMP-ENDS")
                    tot["body_bad"] += 1
                    continue
                if rms < a.min_rms:
                    corr_of[c] = (f"{cc:.3f}", f"{lag:+d}", "silent")
                    continue
                seg_of[c] = (pos + lag, n_out)
                if use_ref:
                    corr_of[c] = (f"{cc:.3f}", f"{lag:+d}", "")
                    continue
                tot["body_n"] += 1
                bad = ""
                if cc < a.min_corr:
                    bad = "CORR"
                if lag0 is None:
                    lag0 = lag
                elif abs(lag - lag0) > a.max_lag_move:
                    bad = (bad + " LAG").strip()
                if bad:
                    tot["body_bad"] += 1
                min_corr = cc if min_corr is None else min(min_corr, cc)
                corr_of[c] = (f"{cc:.3f}", f"{lag:+d}", bad)
        rows = {w["_row"][0]: w["_row"] for w in writes}
        if use_ref:
            ref_rows, ref_key = ref.ring(addr, H)
            print(f"  reference {os.path.basename(ref.trace)}: {len(ref_rows)} ring writes, key-on tick {ref_key}")
            if not ref_rows:
                fails.append("reference has no ring writes (engagement 0)")
            print("  chunk ref_vbl ref_lead_ms ref_over ref_stale  op_lead_ms  d_lead_ms  mix_corr  ref_lag  status")
            rlag0 = None
            rjs, rleads = [], []
            for vbl, tick, c, pos, _v in ref_rows:
                ov, stl, ld = half_reading(pos, c, dh)
                st = []
                if ov:
                    st.append("REF-OVERWRITE")
                if stl:
                    st.append("REF-STALE")
                rl = ld * to_ms
                rjs.append(c - 2)
                rleads.append(rl)
                opl = rows[c][3] if c in rows else None
                mc, ml = "-", "-"
                if ref.wav is not None and ref_key is not None and c in seg_of:
                    p0, n_out = seg_of[c]
                    got = dump.mono[p0: p0 + n_out]
                    rpos = int(round(ref_key / 768.0 + c * dh * o_per_d))
                    cc, lag, _ = correlate(ref.wav, got, rpos, margin=8192 if rlag0 is None else 2048)
                    if cc is not None:
                        tot["body_n"] += 1
                        if rlag0 is None:
                            rlag0 = lag
                        bad = cc < a.min_corr or abs(lag - rlag0) > a.max_lag_move
                        if bad:
                            tot["body_bad"] += 1
                            st.append("MIX-CORR")
                        min_corr = cc if min_corr is None else min(min_corr, cc)
                        mc, ml = f"{cc:.3f}", f"{lag:+d}"
                dl = "-" if opl is None else f"{rl - opl:9.2f}"
                ops = "-" if opl is None else f"{opl:10.2f}"
                print(f"  {c:5d} {vbl:7d} {rl:11.2f} {ov:8d} {stl:9d} {ops:>11} {dl:>10} {mc:>9} {ml:>8}  {' '.join(st) or 'ok'}")
            if len(rjs) >= 2:
                b, a0 = np.polyfit(np.array(rjs, float), np.array(rleads, float), 1)
                print(f"  reference lead fit: {a0:.2f} {b:+.2f}j ms over j = {rjs[0]}..{rjs[-1]}")
            if ref.wav is None:
                fails.append("reference has no mix WAV")
        for c in sorted(set(rows) | set(corr_of)):
            cc, lag, bad = corr_of.get(c, ("-", "-", ""))
            if c in rows:
                _, vbl, lo, lead, over, over_o, stale, slip, step, st = rows[c]
                stt = st + ([bad] if bad and bad not in ("cut", "silent") else [])
                print(f"  {c:5d} {vbl:5d} {lo:#7x} {lead:8.2f} {over:9d} {over_o:9.1f} {stale:9d} "
                      f"{slip:>5} {step:>5} {cc:>5} {lag:>6}  {' '.join(stt) or 'ok'}")
            else:
                print(f"  {c:5d}  (written before key-on)                                   "
                      f"{cc:>5} {lag:>6}  {bad or 'ok'}")
        if len(js) >= 2:
            b, a0 = np.polyfit(np.array(js, float), np.array(leads, float), 1)
            print(f"  lead fit: {a0:.2f} {b:+.2f}j ms over j = {js[0]}..{js[-1]}")
    if tot["over_n"]:
        fails.append(f"{tot['over_n']} overwrites")
    if tot["stale_n"]:
        fails.append(f"{tot['stale_n']} stale refills")
    if tot["slip_steps"]:
        fails.append(f"{tot['slip_steps']} slip steps")
    if tot["wrong_half"]:
        fails.append(f"{tot['wrong_half']} writes in the wrong half")
    if tot["body_bad"]:
        fails.append(f"{tot['body_bad']} chunk bodies fail")
    if dump is not None and tot["body_n"] == 0 and rings:
        fails.append("no chunk body correlated")
    if tot["writes"] == 0 and "no ring run (engagement 0)" not in fails:
        fails.append("0 ring writes (engagement 0)")
    mc = "-" if min_corr is None else f"{min_corr:.3f}"
    verdict = "PASS" if not fails else "FAIL: " + "; ".join(fails)
    print(f"[audio-ring] {tag} runs={len(rings)} ring_writes={tot['writes']} overwrites={tot['over_n']} "
          f"overwrite_ms={tot['over_s']:.1f} overwrite_max_ms={tot['over_max']:.1f} stale={tot['stale_n']} "
          f"stale_ms={tot['stale_s']:.1f} slip_steps={tot['slip_steps']} no_al={tot['no_al']} "
          f"walk_mismatch={tot['walk_bad']} bodies={tot['body_n']} body_min_corr={mc} verdict={verdict}")
    sys.exit(0 if not fails else 2)


if __name__ == "__main__":
    main()
