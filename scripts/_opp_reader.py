"""Reader for the binary .opp performance trace (see core/diag/profile.c).

Stream layout: a 32-byte header, then a mix of variable-length NAME records and
fixed 32-byte PRESENT/SCENE/SPAN/GPUZ/COUNT/FUNC records. Every fixed record carries
a `tid` (the producer thread's index) at byte 28, and spans carry an absolute
microsecond t0, so the file's global order carries no meaning: producers buffer
their own records and the chunks interleave arbitrarily.

Markers:
  * PRESENT is a frame presented to the display, written on the thread that
    presented it: the present thread under eager production, the sim worker
    on the inline path.
  * SCENE (format 3) is a scene published to the scene ring under eager
    production, written on the sim worker. A format 2 trace has no SCENE: its
    PRESENT records under eager were written at scene publication, so their
    cadence is the publication cadence, not the present cadence.
  * The binning threads are the threads that write SCENE, or, in a trace with
    no SCENE, the threads that write PRESENT. Every SCENE or PRESENT on a
    binning thread is a marker (Trace.markers); a PRESENT on another thread
    is a cadence record only.

Binning:
  * SPAN/COUNT raised on a binning thread bin by that thread's stream order,
    to the next marker in it.
  * SPAN/COUNT raised on any other thread bin by interval containment: the
    first marker whose time is at or after the span's t0.
  * GPUZ records are keyed by the RHI frame serial of the frame that submitted
    them, and group per rendered frame, not per marker. A GPUZ raised on a
    binning thread also binds to that marker (the inline render path), so an
    eager-off trace keeps its per-present GPU view.

A trace this reader cannot bin is refused (OppTraceError), never read as a
partial one.
"""
import struct

FORMAT_VERSION = 3
# Format 2 lacks SCENE; it is read with every PRESENT as a marker.
FORMAT_VERSIONS = (2, 3)

REC_NAME, REC_PRESENT, REC_SPAN, REC_GPUZ, REC_COUNT, REC_FUNC, REC_SCENE = \
    1, 2, 3, 4, 5, 6, 7
REC_TID_OFF = 28

KIND_CANONICAL, KIND_INTERMEDIATE, KIND_REPEAT = 0, 1, 2
KIND_NAME = {KIND_CANONICAL: "canonical", KIND_INTERMEDIATE: "intermediate",
             KIND_REPEAT: "repeat"}


class OppTraceError(Exception):
    """The trace cannot be read or binned. Callers print and exit nonzero."""


class Marker:
    """One PRESENT or SCENE record (`rec` is REC_PRESENT or REC_SCENE) and,
    when it is a marker, the spans, counts and GPU zones binned to it."""
    __slots__ = ("rec", "index", "canon_tick", "kind", "t_now_us", "tid",
                 "spans", "counts", "gpuz")

    def __init__(self, rec, index, canon_tick, kind, t_now_us, tid):
        self.rec = rec
        self.index = index
        self.canon_tick = canon_tick
        self.kind = kind
        self.t_now_us = t_now_us
        self.tid = tid
        self.spans = []    # list of (depth, name, t0_us, dur_us, aux, tid)
        self.counts = {}   # name -> value (last wins)
        self.gpuz = []     # list of (depth, name, t0_rel_us, dur_us)


    @property
    def kind_name(self):
        return KIND_NAME.get(self.kind, "?")

    def spans_for_tid(self, tid):
        return [s for s in self.spans if s[5] == tid]


class Trace:
    """names: {id: str}. markers: [Marker] the bins, in time order.
    presents / scenes: every PRESENT / SCENE record, in time order. funcs:
    [dict]. gpu_frames: {frame_serial: [(depth, name, t0_rel_us, dur_us, tid)]}
    - one entry per frame that actually rendered. threads: sorted tid list.
    present_tids / scene_tids: threads that wrote PRESENT / SCENE. bin_tids:
    the binning threads."""
    __slots__ = ("names", "markers", "presents", "scenes", "funcs",
                 "gpu_frames", "threads", "present_tids", "scene_tids",
                 "bin_tids", "version")

    def __init__(self):
        self.names = {}
        self.markers = []
        self.presents = []
        self.scenes = []
        self.funcs = []
        self.gpu_frames = {}
        self.threads = []
        self.present_tids = set()
        self.scene_tids = set()
        self.bin_tids = set()
        self.version = FORMAT_VERSION


def present_busy_us(present):
    """Wall-time union of a present's CPU span intervals (overlap de-duplicated):
    the real CPU cost of the present, independent of scope nesting."""
    spans = present.spans
    if not spans:
        return 0
    iv = sorted((t0, t0 + dur) for (_d, _n, t0, dur, _a, _t) in spans)
    total = 0
    cur_s, cur_e = iv[0]
    for s, e in iv[1:]:
        if s > cur_e:
            total += cur_e - cur_s
            cur_s, cur_e = s, e
        elif e > cur_e:
            cur_e = e
    total += cur_e - cur_s
    return total


def partition_transients(presents, threshold_us):
    """Split presents into (steady, transients) where a transient is any present
    whose CPU busy-time exceeds threshold_us - i.e. a level-load / CD-stall spike
    (~1.4s) rather than a steady-state frame (~1-9ms). threshold_us<=0 disables."""
    if threshold_us <= 0:
        return presents, []
    steady, trans = [], []
    for p in presents:
        (trans if present_busy_us(p) > threshold_us else steady).append(p)
    return steady, trans


def _bin_by_containment(markers, t0_us):
    """First marker whose time is at or after t0_us. A record raised after the
    last marker belongs to the last marker (the tail flush)."""
    lo, hi = 0, len(markers)
    while lo < hi:
        mid = (lo + hi) // 2
        if markers[mid].t_now_us < t0_us:
            lo = mid + 1
        else:
            hi = mid
    return markers[lo] if lo < len(markers) else markers[-1]


def read_opp(path):
    """Parse a format 2 or 3 .opp trace into a Trace. Raises OppTraceError on a
    bad magic, a version this reader does not implement, a corrupt tail, or
    any record it cannot bin."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 32 or data[:4] != b"OPPR":
        raise OppTraceError("%s: not an .opp trace (bad magic)" % path)
    version = struct.unpack_from("<I", data, 4)[0]
    if version not in FORMAT_VERSIONS:
        raise OppTraceError(
            "%s: .opp format version %d, this reader implements versions %s. "
            "Re-capture with the current build (format 2 added per-thread ids "
            "and GPU zones keyed by RHI frame serial, format 3 the SCENE "
            "record)." % (path, version, ", ".join(map(str, FORMAT_VERSIONS))))

    off, n = 32, len(data)
    tr = Trace()
    tr.version = version
    names = tr.names
    # Per-thread record streams, in each thread's own order.
    streams = {}    # tid -> list of ("span"|"count"|"gpuz"|"marker", payload)
    threads = set()
    truncated = False

    def push(tid, item):
        threads.add(tid)
        streams.setdefault(tid, []).append(item)

    while off < n:
        t = data[off]
        if t == REC_NAME:
            if off + 5 > n:
                truncated = True
                break
            nid, ln = struct.unpack_from("<HH", data, off + 1)
            if off + 5 + ln > n:
                truncated = True
                break
            names[nid] = data[off + 5:off + 5 + ln].decode("utf-8", "replace")
            off += 5 + ln
            continue
        if off + 32 > n:
            truncated = True
            break
        tid = data[off + REC_TID_OFF]
        if t == REC_SPAN:
            depth = data[off + 1]
            nid = struct.unpack_from("<H", data, off + 2)[0]
            t0 = struct.unpack_from("<Q", data, off + 8)[0]
            dur, aux = struct.unpack_from("<II", data, off + 16)
            push(tid, ("span", (depth, nid, t0, dur, aux, tid)))
        elif t == REC_COUNT:
            nid = struct.unpack_from("<H", data, off + 2)[0]
            val = struct.unpack_from("<Q", data, off + 8)[0]
            push(tid, ("count", (nid, val)))
        elif t == REC_GPUZ:
            depth = data[off + 1]
            nid = struct.unpack_from("<H", data, off + 2)[0]
            t0rel, dur = struct.unpack_from("<II", data, off + 8)
            serial = struct.unpack_from("<Q", data, off + 16)[0]
            push(tid, ("gpuz", (serial, depth, nid, t0rel, dur, tid)))
        elif t == REC_FUNC:
            nid = struct.unpack_from("<H", data, off + 2)[0]
            addr = struct.unpack_from("<I", data, off + 4)[0]
            self_us, total_us = struct.unpack_from("<QQ", data, off + 8)
            calls = struct.unpack_from("<I", data, off + 24)[0]
            tr.funcs.append({"name": nid, "addr": addr, "calls": calls,
                             "self_us": self_us, "total_us": total_us,
                             "tid": tid})
            threads.add(tid)
        elif t == REC_PRESENT or (t == REC_SCENE and version >= 3):
            kind = data[off + 1]
            idx, canon, _thread_spans = struct.unpack_from("<III", data, off + 4)
            t_now = struct.unpack_from("<Q", data, off + 16)[0]
            p = Marker(t, idx, canon, kind, t_now, tid)
            if t == REC_PRESENT:
                tr.presents.append(p)
                tr.present_tids.add(tid)
            else:
                tr.scenes.append(p)
                tr.scene_tids.add(tid)
            push(tid, ("marker", p))
        else:
            truncated = True
            break
        off += 32

    if truncated:
        raise OppTraceError(
            "%s: corrupt or truncated at byte %d of %d; refusing to report a "
            "partial trace." % (path, off, n))
    tr.bin_tids = set(tr.scene_tids) if tr.scenes else set(tr.present_tids)
    tr.markers = [p for p in tr.presents + tr.scenes if p.tid in tr.bin_tids]
    if not tr.markers:
        raise OppTraceError(
            "%s: no PRESENT or SCENE records; nothing to bin spans into." % path)

    def nm(i):
        if i not in names:
            raise OppTraceError(
                "%s: record cites name id %d, which the trace never interned."
                % (path, i))
        return names[i]

    for fn in tr.funcs:
        fn["name"] = nm(fn["name"])

    tr.markers.sort(key=lambda p: p.t_now_us)
    tr.presents.sort(key=lambda p: p.t_now_us)
    tr.scenes.sort(key=lambda p: p.t_now_us)
    tr.threads = sorted(threads)

    # Binning threads: stream order. Other threads: containment.
    unbinned = 0
    for tid in tr.threads:
        items = streams.get(tid, [])
        if tid in tr.bin_tids:
            pend_spans, pend_counts, pend_gpuz = [], [], []
            for kind, payload in items:
                if kind == "span":
                    pend_spans.append(payload)
                elif kind == "count":
                    pend_counts.append(payload)
                elif kind == "gpuz":
                    pend_gpuz.append(payload)
                else:
                    p = payload
                    for (d, nid, t0, dur, aux, sid) in pend_spans:
                        p.spans.append((d, nm(nid), t0, dur, aux, sid))
                    for (nid, val) in pend_counts:
                        p.counts[nm(nid)] = val
                    for (_s, d, nid, t0r, dur, _t) in pend_gpuz:
                        p.gpuz.append((d, nm(nid), t0r, dur))
                    pend_spans, pend_counts, pend_gpuz = [], [], []
            # Tail after the last marker: the close-time flush. Bind to the last
            # marker rather than drop it.
            last = tr.markers[-1]
            for (d, nid, t0, dur, aux, sid) in pend_spans:
                last.spans.append((d, nm(nid), t0, dur, aux, sid))
            for (nid, val) in pend_counts:
                last.counts[nm(nid)] = val
            for (_s, d, nid, t0r, dur, _t) in pend_gpuz:
                last.gpuz.append((d, nm(nid), t0r, dur))
        else:
            for kind, payload in items:
                if kind == "span":
                    (d, nid, t0, dur, aux, sid) = payload
                    p = _bin_by_containment(tr.markers, t0)
                    if p is None:
                        unbinned += 1
                        continue
                    p.spans.append((d, nm(nid), t0, dur, aux, sid))
                elif kind == "count":
                    unbinned += 0   # counts without a timestamp: see below
                elif kind == "gpuz":
                    pass            # keyed by frame serial only

    # COUNT records from a non-binning thread carry no timestamp, so they bin by
    # their position between that thread's own spans. Walk the stream once more
    # and attribute each to the marker the surrounding spans landed in.
    for tid in tr.threads:
        if tid in tr.bin_tids:
            continue
        cur = None
        for kind, payload in streams.get(tid, []):
            if kind == "span":
                cur = _bin_by_containment(tr.markers, payload[2])
            elif kind == "count":
                (nid, val) = payload
                if cur is None:
                    unbinned += 1
                    continue
                cur.counts[nm(nid)] = val

    # GPU zones: one group per rendered frame, from every thread.
    for tid in tr.threads:
        for kind, payload in streams.get(tid, []):
            if kind != "gpuz":
                continue
            (serial, d, nid, t0r, dur, ztid) = payload
            tr.gpu_frames.setdefault(serial, []).append((d, nm(nid), t0r, dur, ztid))

    if unbinned:
        raise OppTraceError(
            "%s: %d record(s) could not be binned to any marker; refusing to "
            "report a partial trace." % (path, unbinned))

    return tr
