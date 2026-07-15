"""Reader for the binary .opp performance trace (see core/runtime/profile.c).

Stream layout: a 32-byte header, then a mix of variable-length NAME records and
fixed 32-byte PRESENT/SPAN/GPUZ/COUNT records. Binning rule: every SPAN/COUNT
record belongs to the next PRESENT in stream order; GPUZ records carry their own
present_index (GPU results are harvested a few presents late).
"""
import struct

REC_NAME, REC_PRESENT, REC_SPAN, REC_GPUZ, REC_COUNT, REC_FUNC = 1, 2, 3, 4, 5, 6

KIND_CANONICAL, KIND_INTERMEDIATE = 0, 1
KIND_NAME = {KIND_CANONICAL: "canonical", KIND_INTERMEDIATE: "intermediate"}


class Present:
    __slots__ = ("index", "canon_tick", "kind", "t_now_us", "spans", "counts", "gpuz")

    def __init__(self, index, canon_tick, kind, t_now_us):
        self.index = index
        self.canon_tick = canon_tick
        self.kind = kind
        self.t_now_us = t_now_us
        self.spans = []    # list of (depth, name, t0_us, dur_us, aux)
        self.counts = {}   # name -> value (last wins)
        self.gpuz = []     # list of (depth, name, t0_rel_us, dur_us)

    @property
    def kind_name(self):
        return KIND_NAME.get(self.kind, "?")


def read_opp(path):
    """Return (names: dict[int,str], presents: list[Present], funcs: list[dict]).
    Records after the last PRESENT (an incomplete trailing present) are dropped;
    FUNC records are end-of-run per-function aggregates (Tier B)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"OPPR":
        raise ValueError("not an .opp trace (bad magic)")
    version = struct.unpack_from("<I", data, 4)[0]
    off, n = 32, len(data)

    names = {}
    presents = []
    funcs = []
    by_index = {}
    pend_spans, pend_counts = [], []
    all_gpuz = []  # (present_index, depth, name_id, t0_rel_us, dur_us)

    def nm(i):
        return names.get(i, "#%d" % i)

    while off + 1 <= n:
        t = data[off]
        if t == REC_NAME:
            if off + 5 > n:
                break
            nid, ln = struct.unpack_from("<HH", data, off + 1)
            s = data[off + 5:off + 5 + ln].decode("utf-8", "replace")
            names[nid] = s
            off += 5 + ln
        elif t == REC_SPAN:
            depth = data[off + 1]
            nid = struct.unpack_from("<H", data, off + 2)[0]
            t0 = struct.unpack_from("<Q", data, off + 8)[0]
            dur, aux = struct.unpack_from("<II", data, off + 16)
            pend_spans.append((depth, nid, t0, dur, aux))
            off += 32
        elif t == REC_COUNT:
            nid = struct.unpack_from("<H", data, off + 2)[0]
            val = struct.unpack_from("<Q", data, off + 8)[0]
            pend_counts.append((nid, val))
            off += 32
        elif t == REC_FUNC:
            nid = struct.unpack_from("<H", data, off + 2)[0]
            addr = struct.unpack_from("<I", data, off + 4)[0]
            self_us, total_us = struct.unpack_from("<QQ", data, off + 8)
            calls = struct.unpack_from("<I", data, off + 24)[0]
            funcs.append({"name": nm(nid), "addr": addr, "calls": calls,
                          "self_us": self_us, "total_us": total_us})
            off += 32
        elif t == REC_GPUZ:
            depth = data[off + 1]
            nid = struct.unpack_from("<H", data, off + 2)[0]
            pidx, t0rel, dur = struct.unpack_from("<III", data, off + 4)
            all_gpuz.append((pidx, depth, nid, t0rel, dur))
            off += 32
        elif t == REC_PRESENT:
            kind = data[off + 1]
            idx, canon, _spancount = struct.unpack_from("<III", data, off + 4)
            t_now = struct.unpack_from("<Q", data, off + 16)[0]
            p = Present(idx, canon, kind, t_now)
            for (depth, nid, t0, dur, aux) in pend_spans:
                p.spans.append((depth, nm(nid), t0, dur, aux))
            for (nid, val) in pend_counts:
                p.counts[nm(nid)] = val
            presents.append(p)
            by_index[idx] = p
            pend_spans, pend_counts = [], []
            off += 32
        else:
            break  # corrupt / truncated tail

    for (pidx, depth, nid, t0rel, dur) in all_gpuz:
        p = by_index.get(pidx)
        if p is not None:
            p.gpuz.append((depth, nm(nid), t0rel, dur))

    return names, presents, funcs
