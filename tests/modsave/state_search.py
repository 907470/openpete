#!/usr/bin/env python3
"""Exhaustive state-model test of the per-mod save-record rules.

Breadth-first search over canonicalized states of the C module
(tests/modsave/modsave_model.c, loaded through ctypes) with a Python
reference model applied to every edge alongside it: model_ref.py, or with
--ref indep (or MODSAVE_REF=indep) model_ref_indep.py, a second reference
written apart from both model_ref.py and the C module. A run
is a list of suites; each suite fixes the domain, a setup prefix, the
event alphabet and the depth bound. On every edge the search requires:

  reference   the C effects equal the reference model's effects;
  shape       the two canonical forms stay in bijection (a C state merged
              with another must merge in the reference too, and back);
and on the reference state, invariants written apart from both models:
  I1  no read callback on a host event (savestates, rewind, runahead,
      handoff, hot reload, toggles: timeline restores issue none);
  I2  after a tick, every active mod's held identity equals the current
      identity (a read fires whenever they differ, and a write leaves
      none pending), except after a reset or a load binding, whose new
      identity the next tick's delivery arm serves, and after a write
      completion, where only the mods enabled at its issue take the new
      identity;
  I3  at most one read per mod per tick;
  I4  the card image changes only by the game's own writes, a restore of
      led writes, a process restart (the image is unread until first
      access) or the card's first access, which reads the file with any
      outside edit made before it (the engine never writes the card);
  I5  no index holds two snapshots with one (hash, lineage), nor more than
      the cap;
  I6  round trip: an OK, GAP or STALE read delivers a value the mod
      returned from a completed write of the card slot; an OK read of the
      record the index last completed delivers that write's value when
      the mod wrote it;
  I7  a playthrough the path started with a reset (no load resolved
      since) leaves every active mod at the fresh value until the mod
      plays, from the first tick after the reset.

Usage: state_search.py --lib PATH [--suite NAME ...] [--quick] [--fault N]
                       [--ref model_ref|indep] [--list]
  --quick      each suite at its quick depth (the ctest default)
  --ref        the reference model (default model_ref, or MODSAVE_REF); faults
               2 and 3 patch model_ref internals and need --ref model_ref
  --fault N    calibration: seed defect N (see FAULTS) and require that the
               search reports it
Exit 0 every suite passed with nonzero engagement, 1 a check failed,
64 usage.
"""
import argparse
import ctypes
import importlib
import os
import sys
import time
from collections import Counter
from dataclasses import replace

import model_ref as M

CONTENT = {"E": 0, "X": 1, "Y": 2, "P": 3, "T": 4}
OUTCOME = {"ok": 0, "fail": 1, "torn": 2}


# ── C side ────────────────────────────────────────────────────────────────

class CModel:
    def __init__(self, path):
        self.lib = ctypes.CDLL(path)
        self.lib.msm_state_size.restype = ctypes.c_size_t
        self.size = self.lib.msm_state_size()
        self.lib.msm_canon.restype = ctypes.c_size_t
        self.lib.msm_event_name.restype = ctypes.c_char_p
        self.kind = {}
        for k in range(64):
            n = self.lib.msm_event_name(k)
            if n is None:
                break
            self.kind[n.decode()] = k
        self.fx = ctypes.create_string_buffer(4096)
        self.canon_buf = ctypes.create_string_buffer(8192)
        self.fault = ctypes.c_int.in_dll(self.lib, "msm_seed_fault")

    def init(self, cfg, loaded):
        st = ctypes.create_string_buffer(self.size)
        c = (ctypes.c_uint8 * 4)(cfg.n_mods, cfg.n_slots, cfg.n_idx, cfg.cap)
        mask = sum(1 << m for m in loaded)
        self.lib.msm_init(st, c, ctypes.c_uint8(mask))
        return st.raw

    def encode(self, ev):
        kind, args = ev[0], ev[1:]
        a = [0, 0, 0, 0]
        if kind in ("WRITE", "ISSUE"):
            a = [args[0], args[1], CONTENT[args[2]], sum(1 << m for m in args[3])]
        elif kind in ("WRITE_FAIL", "EDIT"):
            a = [args[0], args[1], CONTENT[args[2]], 0]
        elif kind == "COMPLETE":
            a = [args[0], OUTCOME[args[1]], 0, 0]
        elif kind in ("RESTART", "HANDOFF"):
            a = [sum(1 << m for m in args[0]), sum(1 << m for m in args[1]), 0, 0]
        elif kind == "MODSET":
            a = [args[0], 0 if args[1] is None else 1 + sum(1 << m for m in args[1]), 0, 0]
        else:
            for i, v in enumerate(args):
                a[i] = v
        return (ctypes.c_uint8 * 5)(self.kind[kind], *a)

    def apply(self, blob, ev):
        st = ctypes.create_string_buffer(blob, self.size)
        e = self.encode(ev)
        if self.lib.msm_apply(st, e, self.fx, ctypes.c_size_t(len(self.fx))) != 0:
            raise RuntimeError("effect buffer overflow")
        fx = self.fx.value.decode()
        return st.raw, (fx.split(";") if fx else [])

    def enabled(self, blob, ev):
        st = ctypes.create_string_buffer(blob, self.size)
        return bool(self.lib.msm_enabled(st, self.encode(ev)))

    def canon(self, blob):
        st = ctypes.create_string_buffer(blob, self.size)
        n = self.lib.msm_canon(st, self.canon_buf, ctypes.c_size_t(len(self.canon_buf)))
        if n == 0:
            raise RuntimeError("canon buffer too small")
        return self.canon_buf.raw[:n]


# ── seeded defects (calibration) ──────────────────────────────────────────

def _fault_held(orig):
    def complete(s, slot, outcome):
        b = s.blk
        held = b.held
        orig(s, slot, outcome)
        if outcome == "ok" and s.blk.held != held:
            s.blk = replace(s.blk, held=held)
    return complete


def _fault_reset(s):
    M.deliver(s)
    if s.armed:
        s.blk = replace(s.blk, gen=s.blk.gen + 1, legacy=False)


def _fault_cross():
    orig = M.status_for

    def status_for(s, b, m):
        return orig(s, b, 1 - m)
    M.status_for = status_for


# Reference models: model_ref.py, and model_ref_indep.py, a second reference
# written from the design text apart from both model_ref.py and the C module.
REFS = {"model_ref": "model_ref", "indep": "model_ref_indep"}

FAULTS = {
    1: ("C only: GAP and OK swapped in the status precedence", "reference", None),
    2: ("both models: a write completion leaves the held identities unset", "I2",
        lambda: setattr(M, "complete", _fault_held(M.complete))),
    3: ("both models: a new-playthrough reset keeps the key", "I7",
        lambda: M.RULES.__setitem__("RESET", (M.TICK, M.g_tick, _fault_reset))),
    4: ("both models: a read delivers the other mod's record", "I6", _fault_cross),
}


# ── suites ────────────────────────────────────────────────────────────────

def ev_writes(slots, idxs, contents, masks=((),)):
    return [("WRITE", s, i, c, f) for s in slots for i in idxs for c in contents for f in masks]


def ev_loads(slots, idxs):
    return [("LOAD", s, i) for s in slots for i in idxs]


CORE = [("T",), ("RESET",)]


def suites():
    """name -> (Config, loaded mods, setup events, alphabet, quick depth, full depth)."""
    c1 = M.Config(n_mods=2, n_slots=1, n_idx=2, cap=4)
    c2 = M.Config(n_mods=2, n_slots=2, n_idx=1, cap=4)
    c1i = M.Config(n_mods=2, n_slots=1, n_idx=1, cap=4)
    m0 = [("MOUNT", 0)]
    m01 = [("MOUNT", 0), ("MOUNT", 1)]
    play = [("PLAY", m, 1) for m in (0, 1)]
    return {
        # Delivery rule (V25-V27): loads, resets, writes, play and toggles.
        "delivery": (c1, (0, 1), m0,
                     CORE + ev_loads([0], [0, 1]) + ev_writes([0], [0, 1], "XY") + play
                     + [("TOGGLE", 0), ("TOGGLE", 1)], 5, 6),
        # Carry-forward, GAP, refuse-newer and -1 writes over one index.
        "carry": (c1i, (0, 1), m0,
                  CORE + ev_loads([0], [0]) + ev_writes([0], [0], "XY", ((), (1,)))
                  + [("PLAY", 1, 1), ("PLAY", 1, 2), ("TOGGLE", 1), ("SCHEMA", 1)], 6, 8),
        # Snapshot eviction across two lineages (depth >= 8 by the plan).
        "eviction": (c1i, (0,), m0,
                     [("RESET",), ("LOAD", 0, 0), ("PLAY", 0, 1), ("PLAY", 0, 2)]
                     + ev_writes([0], [0], "XYP"), 8, 10),
        # LEGACY_PAD (own, foreign, zero), outside edits (STALE), an older card
        # restored (EDIT back to an earlier content) and carry from the
        # lineage's newest snapshot.
        "legacy": (c1i, (0, 1), [("EDIT", 0, 0, "P"), ("RESTART", (0, 1), (0, 1)), ("MOUNT", 0)],
                   CORE + ev_loads([0], [0]) + ev_writes([0], [0], "XP")
                   + [("PLAY", 0, 1), ("PLAY", 1, 1), ("EDIT", 0, 0, "X"), ("EDIT", 0, 0, "Y"),
                      ("EDIT", 0, 0, "P"), ("RESTART", (0, 1), (0, 1)), ("MOUNT", 0)], 7, 8),
        # Two card slots: multi-match, stale selection, port rebind, the
        # modset mismatch. Setup: both cards hold record X written by an
        # unarmed process, so no sidecar holds it.
        "ports": (c2, (), [("MOUNT", 0), ("MOUNT", 1), ("WRITE", 0, 0, "X", ()),
                           ("WRITE", 1, 0, "X", ()), ("RESTART", (0,), (0,)), ("MOUNT", 0),
                           ("MOUNT", 1)],
                  CORE + ev_loads([0, 1], [0]) + [("LOAD_STALE", s, 0) for s in (0, 1)]
                  + ev_writes([0, 1], [0], "XY") + [("PLAY", 0, 1), ("SS_TAKE",), ("SS_LOAD",),
                                                     ("RESTART", (0,), (0,)), ("RESTART", (0,), ()),
                                                     ("ENTER", 0), ("MOUNT", 0), ("MOUNT", 1)], 5, 6),
        # Savestates (same process, older blob), handoff, rewind.
        "timeline": (c1i, (0, 1), m0,
                     CORE + ev_loads([0], [0]) + ev_writes([0], [0], "X") + play
                     + [("SS_TAKE",), ("SS_LOAD",), ("SS_LOAD_OLD",), ("RW_MARK",), ("REWIND",),
                        ("HANDOFF", (0, 1), (0, 1)), ("HANDOFF", (0,), (0,)), ("MOUNT", 0)], 5, 8),
        # Runahead: rollback, adoption, failed adoption flush, partial restore.
        "runahead": (c1i, (0,), m0,
                     CORE + ev_loads([0], [0]) + ev_writes([0], [0], "XY") + [("PLAY", 0, 1)]
                     + [("RA_BEGIN",), ("RA_ROLLBACK",), ("RA_ADOPT",), ("RA_ADOPT_FAIL",),
                        ("RA_PARTIAL",)], 6, 9),
        # Asynchronous writes (V35): issue and completion apart, with every
        # event the design names between them.
        "async": (c1i, (0, 1), m0,
                  CORE + ev_loads([0], [0]) + [("ISSUE", 0, 0, c, ()) for c in "XE"]
                  + [("COMPLETE", 0, o) for o in ("ok", "fail", "torn")]
                  + [("ISSUE_NOREC", 0), ("ABORT", 0, 0)]
                  + [("PLAY", 0, 1), ("TOGGLE", 1), ("SS_TAKE",), ("SS_LOAD",), ("SS_LOAD_OLD",),
                     ("RESTART", (0, 1), (0, 1)), ("MOUNT", 0)], 6, 9),
        # Process lifecycle: mods init with 0, 1, 2 mods, deferred entry, hot
        # reload, mount with and without a .modsave, seeding, --adopt-saves,
        # the legacy .modset tag, sidecar faults, format and file create.
        # Setup: record X written by an unarmed process (no sidecar), then an
        # armed process saves Y at index 1 (the card's first .modsave, X seeded).
        "process": (c1, (), [("MOUNT", 0), ("WRITE", 0, 0, "X", ()), ("RESTART", (0,), (0,)),
                             ("MOUNT", 0), ("WRITE", 0, 1, "Y", ())],
                    [("T",), ("LOAD", 0, 0), ("LOAD", 0, 1), ("WRITE", 0, 1, "X", ()),
                     ("WRITE", 0, 1, "Y", ()), ("RESTART", (), ()),
                     ("RESTART", (0,), ()), ("RESTART", (0, 1), (0, 1)), ("ENTER", 0), ("ENTER", 1),
                     ("MOUNT", 0), ("MOUNT_ADOPT", 0), ("MODSET", 0, (0,)), ("SC_FAULT",),
                     ("SC_CORRUPT", 0), ("FORMAT", 0), ("FILE_CREATE", 0), ("WRITE_FAIL", 0, 0, "Y")],
                    7, 8),
    }


# Engagement (LAW §8): effects each suite must produce at least once, or it
# reports FAIL as vacuous for the rules it exists to reach.
EXPECT = {
    "delivery": ("OK", "ABSENT", "put", "front"),
    "carry": ("GAP", "NEWER", "L:refuse-newer"),
    "eviction": ("evict", "OK", "ABSENT"),
    "legacy": ("LEGACY_PAD", "STALE", "L:carry-from-newest"),
    "ports": ("L:multimatch-lowest", "L:mismatch"),
    "timeline": ("OK", "ABSENT", "L:write-key-unknown"),
    "runahead": ("snap", "L:adoption-flush-failed", "STALE"),
    "async": ("drop", "OK", "ABSENT", "L:completion-no-entry"),
    "process": ("seed", "format", "adopt", "L:sidecar-unparseable", "L:sidecar-write-failed",
                "L:flush-failed", "STALE"),
}


# ── invariants (on the reference state, apart from both models' rules) ──

class Truth:
    """Path facts the invariants need, rolled back with the block."""
    __slots__ = ("origin", "played", "wrote", "last", "stack")

    def __init__(self, n_mods):
        self.origin = "boot"           # 'reset' | 'load' | 'boot'
        self.played = (False,) * n_mods
        self.wrote = frozenset()       # (slot, mod, value) of completed writes
        self.last = {}                 # (slot, idx) -> (content, {mod: value})
        self.stack = {}                # 'ss' | 'rw' | 'spec' -> (origin, played)

    def copy(self):
        t = Truth(0)
        t.origin, t.played, t.wrote = self.origin, self.played, self.wrote
        t.last, t.stack = dict(self.last), dict(self.stack)
        return t


def _held_ok(s, mods=None):
    """Every active mod (or every mod in @p mods) holds the current identity."""
    cur = M.current_identity(s.blk)
    if cur is None or not s.armed:
        return True
    want = mods if mods is not None else [m for m in range(s.cfg.n_mods) if M.active(s, m)]
    return all(s.blk.held[m] == cur for m in want)


def _card_change_ok(ev, before, after):
    if before.s_card == after.s_card:
        return True
    kind = ev[0]
    if kind in ("WRITE", "WRITE_FAIL", "COMPLETE", "FILE_CREATE", "FORMAT"):
        changed = {(c, i) for c in range(before.cfg.n_slots) for i in range(before.cfg.n_idx)
                   if before.s_card[c][i] != after.s_card[c][i]}
        if kind in ("WRITE", "WRITE_FAIL"):
            return changed <= {(ev[1], ev[2])}
        if kind == "COMPLETE":
            return all(c == ev[1] for c, _ in changed)
        return all(c == ev[1] for c, _ in changed)
    if kind in ("MOUNT", "MOUNT_ADOPT"):
        slot = ev[1]
        return not before.mounted[slot] and after.s_card[slot] == after.s_disk[slot] and all(
            before.s_card[c] == after.s_card[c] for c in range(before.cfg.n_slots) if c != slot)
    return kind in ("RA_ROLLBACK", "RA_PARTIAL", "RESTART", "HANDOFF")


def _snaps_ok(s):
    for t in list(s.sc_mem) + list(s.sc_disk):
        if t in (None, "CORRUPT"):
            continue
        for snaps in t:
            if len(snaps) > s.cfg.cap:
                return False
            keys = [(x.content, x.lin) for x in snaps]
            if len(set(keys)) != len(keys):
                return False
    return True


def _truth_step(t, ev, before, after, fx):
    """Advance the path facts over one edge."""
    t = t.copy()
    kind = ev[0]
    if kind == "SS_TAKE":
        t.stack["ss"] = (t.origin, t.played)
    elif kind == "RW_MARK":
        t.stack["rw"] = (t.origin, t.played)
    elif kind == "RA_BEGIN":
        t.stack["spec"] = (t.origin, t.played)
        t.stack["spec_writes"] = (t.wrote, dict(t.last))
    elif kind in ("SS_LOAD", "SS_LOAD_OLD") and "ss" in t.stack:
        t.origin, t.played = t.stack["ss"]
    elif kind == "HANDOFF":
        t.stack["ss"] = (t.origin, t.played)
    elif kind == "REWIND":
        t.origin, t.played = t.stack["rw"]
    elif kind == "RA_ROLLBACK":
        t.origin, t.played = t.stack["spec"]
    if kind in ("RA_ROLLBACK", "RA_ADOPT_FAIL", "RA_PARTIAL"):
        # Led writes whose snapshots were discarded never produced a record.
        t.wrote, last = t.stack["spec_writes"]
        t.last = dict(last)
    elif kind == "RESTART":
        t.origin, t.played = "boot", (False,) * before.cfg.n_mods
    if kind == "SS_LOAD_OLD":
        t.origin = "boot"
    resets = 1 if kind == "RESET" and before.armed else 0
    if before.blk.pload is not None and M.is_tick(ev) and after.blk.gen - before.blk.gen > resets:
        t.origin, t.played = "load", (False,) * before.cfg.n_mods
    if kind == "RESET" and before.armed:
        t.origin, t.played = "reset", (False,) * before.cfg.n_mods
    if kind == "PLAY":
        t.played = M.tset(t.played, ev[1], True)
    _truth_writes(t, ev, before, after, fx)
    return t


def _truth_writes(t, ev, before, after, fx):
    if ev[0] not in ("WRITE", "COMPLETE") or not before.armed:
        return
    slot = ev[1]
    e = before.blk.entries[slot] if ev[0] == "COMPLETE" else None
    if ev[0] == "COMPLETE" and (e is None or ev[2] != "ok" or not e.captured or e.norec):
        return
    idx = ev[2] if ev[0] == "WRITE" else e.idx
    content = ev[3] if ev[0] == "WRITE" else e.content
    if content == "E":
        t.last.pop((slot, idx), None)
        return
    if ev[0] == "WRITE":
        vals = {m: after.blk.arena[m] for m in range(before.cfg.n_mods)
                if f"W{m}" in fx and m not in ev[4]}
    else:
        vals = {m: r.payload for m, r in enumerate(e.recs) if r is not None and not r.gap}
    t.wrote = t.wrote | {(slot, m, v) for m, v in vals.items()}
    t.last[(slot, idx)] = (content, vals)


WRITES = ("WRITE", "WRITE_FAIL", "COMPLETE", "FILE_CREATE", "ISSUE")


def check_edge(ev, before, after, fx, t_before, t_after):
    """Invariant violations of one edge, as messages."""
    bad = []
    reads = [x for x in fx if x.startswith("R")]
    if reads and not M.is_tick(ev):
        bad.append(f"I1 read on host event: {reads}")
    if M.is_tick(ev) and ev[0] not in ("RESET", "LOAD", "LOAD_STALE", "COMPLETE") and not _held_ok(after):
        bad.append("I2 held identity differs from current after a tick")
    if ev[0] == "COMPLETE" and ev[2] == "ok":
        e = before.blk.entries[ev[1]]
        if e is not None and e.captured and e.lin is not None and e.gen == after.blk.gen \
                and not _held_ok(after, e.en):
            bad.append("I2 a mod enabled at issue does not hold the completed write's identity")
    mods = Counter(x[1] for x in reads)
    if any(n > 1 for n in mods.values()):
        bad.append(f"I3 two reads for one mod in a tick: {reads}")
    if not _card_change_ok(ev, before, after):
        bad.append("I4 card image changed by a non-game event")
    if not _snaps_ok(after):
        bad.append("I5 duplicate (hash, lineage) or cap exceeded")
    bad += _roundtrip(after, reads, t_before, ev[0] in WRITES)
    bad += _fresh(after, ev, t_after)
    return bad


def _roundtrip(after, reads, t, wrote_this_tick):
    """I6 on the reads of one tick. The reads ran at the delivery arm; when
    the tick's action is a write the key afterwards may name another save,
    so only the slot-free form is checked then."""
    bad = []
    b = after.blk
    if not isinstance(b.key, tuple):
        return bad
    slot, idx, _, content = b.key
    for r in reads:
        m, status, _, pay = r[1:].split(":")
        m = int(m)
        if status not in ("OK", "GAP", "STALE"):
            continue
        slots = {x[0] for x in t.wrote if x[1] == m and x[2] == int(pay)}
        if not slots:
            bad.append(f"I6 {r}: mod {m} never wrote this payload")
            continue
        if wrote_this_tick:
            continue
        if slot not in slots:
            bad.append(f"I6 {r}: payload never written by mod {m} on slot {slot}")
        last = t.last.get((slot, idx))
        if status == "OK" and last and last[0] == content and m in last[1] \
                and int(pay) != last[1][m]:
            bad.append(f"I6 {r}: the last write of this record stored {last[1][m]}")
    return bad


def _fresh(after, ev, t):
    if t.origin != "reset" or not M.is_tick(ev) or ev[0] == "RESET" or not after.armed:
        return []
    return [f"I7 mod {m} holds {after.blk.arena[m]} in a playthrough started by a reset"
            for m in range(after.cfg.n_mods)
            if M.active(after, m) and not t.played[m] and after.blk.arena[m] != 0]


# Engagement of single rules (LAW §8): edge kinds each suite must apply at
# least once, counted by edge_marks().
ENGAGE = {
    "legacy": ("edit-before-first-access",),
    "async": ("toggle-while-pending", "torn-completion", "norec-completion", "abort"),
}


def edge_marks(ev, before):
    """The ENGAGE marks one edge earns: an outside edit of a card file the
    process has not read yet (its first access must see it), an issue
    a mod toggled while a write entry is pending (the enabled set at issue
    governs its completion), a completion whose record fails the checksum
    verdict."""
    marks = []
    if ev[0] == "EDIT" and not before.mounted[ev[1]]:
        marks.append("edit-before-first-access")
    if ev[0] == "TOGGLE" and any(e is not None for e in before.blk.entries):
        marks.append("toggle-while-pending")
    if ev[0] == "COMPLETE" and ev[2] == "torn" and before.blk.entries[ev[1]] is not None:
        marks.append("torn-completion")
    e = before.blk.entries[ev[1]] if ev[0] == "COMPLETE" else None
    if e is not None and e.norec and not e.orphan and ev[2] == "ok":
        marks.append("norec-completion")
    if ev[0] == "ABORT":
        marks.append("abort")
    return marks


def _short(ev):
    return ev[0] + ("(" + ",".join(str(x) for x in ev[1:]) + ")" if len(ev) > 1 else "")


# ── search ────────────────────────────────────────────────────────────────

def run_suite(cm, name, spec, depth, fault):
    cfg, loaded, setup, alphabet, _, _ = spec
    py = M.initial(cfg, loaded)
    c = cm.init(cfg, loaded)
    truth = Truth(cfg.n_mods)
    for ev in setup:
        py2, fx_py = M.apply(py, ev)
        c, fx_c = cm.apply(c, ev)
        truth = _truth_step(truth, ev, py, py2, fx_py)
        py = py2
    seen = {cm.canon(c): M.canon(py)}
    seen_py = {M.canon(py)}
    frontier = [(py, c, truth, ())]
    stats = Counter()
    statuses = Counter()
    kinds = Counter()
    marks = Counter()
    failures = []
    t0 = time.time()
    for d in range(depth):
        nxt = []
        for py, c, truth, path in frontier:
            for ev in alphabet:
                ok_py = M.enabled(py, ev)
                if ok_py != cm.enabled(c, ev):
                    failures.append(f"reference: precondition of {ev} differs")
                    continue
                if not ok_py:
                    continue
                py2, fx_py = M.apply(py, ev)
                c2, fx_c = cm.apply(c, ev)
                stats["edges"] += 1
                kinds[ev[0]] += 1
                marks.update(edge_marks(ev, py))
                where = " <- " + " ".join(_short(x) for x in path + (ev,))
                if fx_py != fx_c:
                    failures.append(f"reference: {ev} effects C {fx_c} vs reference {fx_py}{where}")
                for x in fx_py:
                    if x.startswith("R"):
                        statuses[x.split(":")[1]] += 1
                    else:
                        statuses[x.split(":")[-1] if x[0] in "SQ" else x] += 1
                    stats["effects"] += 1
                t2 = _truth_step(truth, ev, py, py2, fx_py)
                for msg in check_edge(ev, py, py2, fx_py, truth, t2):
                    failures.append(f"{msg}{where}")
                stats["invariant_checks"] += 7
                kc, kp = cm.canon(c2), M.canon(py2)
                if kc in seen:
                    if seen[kc] != kp:
                        failures.append(f"shape: C merges states the reference distinguishes at {ev}")
                    continue
                if kp in seen_py:
                    failures.append(f"shape: reference merges states C distinguishes at {ev}")
                seen[kc] = kp
                seen_py.add(kp)
                nxt.append((py2, c2, t2, path + (ev,)))
                if len(failures) > 20:
                    break
            if len(failures) > 20:
                break
        frontier = nxt
        stats[f"depth{d + 1}"] = len(seen)
        if len(failures) > 20 or not frontier:
            break
    stats["states"] = len(seen)
    stats["seconds"] = round(time.time() - t0, 1)
    return stats, statuses, kinds, marks, failures


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--lib", required=True)
    ap.add_argument("--suite", action="append")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--fault", type=int, default=0)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--ref", choices=sorted(REFS), default=os.environ.get("MODSAVE_REF", "model_ref"))
    a = ap.parse_args(argv[1:])
    global M
    M = importlib.import_module(REFS[a.ref])
    print(f"[state-model] reference: {REFS[a.ref]}")
    table = suites()
    if a.list:
        for n, sp in table.items():
            print(f"{n}: {len(sp[3])} events, depth {sp[4]} quick / {sp[5]} full")
        for q, txt in M.OPEN_QUESTIONS.items():
            print(f"{q}: {txt}")
        return 0
    cm = CModel(a.lib)
    if a.fault:
        if a.ref != "model_ref" and a.fault in (2, 3):
            print(f"fault {a.fault} patches model_ref internals; run it with --ref model_ref")
            return 64
        if a.fault not in FAULTS:
            print(f"unknown fault {a.fault}")
            return 64
        cm.fault.value = a.fault
        if FAULTS[a.fault][2]:
            FAULTS[a.fault][2]()
    names = a.suite or list(table)
    total_fail = 0
    caught = set()
    for n in names:
        sp = table[n]
        depth = sp[4] if a.quick else sp[5]
        stats, statuses, kinds, marks, failures = run_suite(cm, n, sp, depth, a.fault)
        engaged = stats["edges"] > 0 and stats["effects"] > 0
        print(f"[state-model] {n}: depth {depth}, {stats['states']} states, {stats['edges']} edges, "
              f"{stats['effects']} effects compared, {stats['seconds']} s")
        print(f"[state-model]   effects seen: {dict(sorted(statuses.items()))}")
        missing = [x for x in EXPECT.get(n, ()) if not statuses[x]]
        if missing:
            failures.append(f"engagement: never produced {missing}")
        print(f"[state-model]   events applied: {dict(sorted(kinds.items()))}")
        if n in ENGAGE:
            print(f"[state-model]   rule edges: {dict((x, marks[x]) for x in ENGAGE[n])}")
            unmet = [x for x in ENGAGE[n] if not marks[x]]
            if unmet:
                failures.append(f"engagement: never applied {unmet}")
        verdict = "FAIL" if failures else ("PASS" if engaged else "VACUOUS")
        print(f"[state-model]   verdict: {verdict}")
        for f in failures[:5]:
            print(f"[state-model]   {f}")
        caught.update(f.split(" ")[0].rstrip(":") for f in failures)
        if failures or not engaged:
            total_fail += 1
    if a.fault:
        want = FAULTS[a.fault][1]
        hit = want in caught
        print(f"[state-model] calibration fault {a.fault} ({FAULTS[a.fault][0]}): "
              f"{'caught by ' + want if hit else 'NOT caught by ' + want} (checks that fired: {sorted(caught)})")
        return 0 if hit else 1
    print(f"[state-model] verdict: {'PASS' if total_fail == 0 else 'FAIL'}")
    return 0 if total_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
