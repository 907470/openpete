"""Second reference model of the per-mod save-record rules.

A second Python reference for tests/modsave/state_search.py (--ref indep),
written from the save-record design's rules (status precedence, the key's
block and its events, write capture, the load binding, the delivery rule)
apart from modsave_model.c and model_ref.py, in a different representation.
It speaks the effect grammar of modsave_model.h and exposes the state
attributes the search's invariants read (blk.key, blk.held, blk.gen,
blk.pload, blk.entries, blk.arena, s_card, sc_mem, sc_disk, armed, cfg).

Representation: every value is immutable (tuples and NamedTuples); a state
is an object of such fields, copied field by field before an event is
applied, so apply() never mutates its input. A lineage is (root, n): a new
root minted from entropy wherever a fresh lineage is consumed (a write
from a cleared or unknown key, a load of an index with no snapshot,
seeding), n always 0.
A record content is one of E (empty), X, Y, P (legacy span holding mod 0's
magic), T (torn, checksum fails); the content stands for its hash.

READINGS lists each point the rules leave open and the reading this model
takes. On the ids in ALIGNED the model takes modsave_model.c's choice where
it differs from its own reading; MODSAVE_INDEP_OWN=1 runs every own reading,
MODSAVE_INDEP_OWN=R1,R9 the named ones.
"""
import os
from typing import NamedTuple, Optional


class Config(NamedTuple):
    n_mods: int
    n_slots: int
    n_idx: int
    cap: int


class Rec(NamedTuple):
    schema: int
    gap: int
    payload: int


class Snap(NamedTuple):
    content: str
    lin: tuple
    tag: int                 # mask of entered mods at the write
    recs: tuple              # per mod id: Rec or None


class Entry(NamedTuple):
    captured: bool           # armed at issue: the capture runs
    slot: int
    idx: int
    content: str
    gen: int
    lin: Optional[tuple]     # None for an empty-record entry or uncaptured
    fresh: bool
    recs: Optional[tuple]
    tag: int
    en: tuple = ()           # mods enabled at issue: they take the held identity


class Block(NamedTuple):
    key: object              # "unknown" | "cleared" | (slot, idx, lin, content)
    gen: int
    legacy: bool             # legacy copy non-zero
    held: tuple              # per mod id: (lin, gen) or None
    pload: object            # None | (cands frozenset, legacy, content, chosen)
    entries: tuple           # per slot: Entry or None
    arena: tuple             # per mod id: abstract value 0..2


UNKNOWN, CLEARED, CORRUPT = "unknown", "cleared", "CORRUPT"
TICKS = frozenset(("T", "LOAD", "LOAD_STALE", "RESET", "WRITE", "WRITE_FAIL", "ISSUE",
                   "COMPLETE", "FILE_CREATE", "FORMAT", "PLAY"))
SPEC_HOST = frozenset(("RA_ROLLBACK", "RA_ADOPT", "RA_ADOPT_FAIL", "RA_PARTIAL"))

READINGS = {
    "R1": "fresh lineage of a write: minted from entropy at issue and held in the entry "
          "(round 15: no pending-lineage chain)",
    "R2": "a savestate load leaves the card image as it is",
    "R3": "a committed card write clears the rewind keyframe, so a rewind never spans a "
          "committed save and replay dedupe is not reachable",
    "R4": "arena after a read: OK, GAP and STALE take the payload; ABSENT and NEWER the fresh "
          "value 0; LEGACY_PAD imports value 2 for mod 0 (content P carries its magic), 0 for "
          "any other mod",
    "R5": "a write from a set key carries the key's lineage at any index; the carry source is "
          "looked up at the key's own (slot, index) (V48)",
    "R6": "an empty-record write leaves the key as it is and drops the index's snapshots",
    "R7": "seeding runs at the first captured write or format of a card with no .modsave and "
          "snapshots every non-empty index the write does not cover",
    "R8": "the modset mismatch compares the resolved source snapshot's tag with the session tag "
          "(entered mods) at the delivery arm; no source, no line",
    "R9": "failed adoption flush: own, the dirty marks stay; aligned, they are cleared and the "
          "line is logged only when a slot was dirty or an op was queued",
    "R10": "handoff: own, the arena of a mod entered in both processes is carried, others "
           "start fresh; aligned, every mod id's arena row is carried; the savestate slot then "
           "holds the handed-off state",
    "R11": "port rebind from the state's card section: the key names the port holding the card "
           "the state held at its slot; the model's cards never change ports, so it stands",
    "R12": "outside edit: the card image is read at the card's first access (MOUNT), not at "
           "process start, so an edit before it is seen",
    "R13": "one write in flight per port (V45): no issue while the slot's entry is pending",
    "R14": "deferred first entry: own, the mod's arena is reset to the fresh value; aligned, it "
           "keeps the arena the block holds",
    "R15": "savestate load: own, the rewind keyframe stays; aligned, it is cleared",
    "R16": "seeded lineages: a new root each",
    "R17": "--adopt-saves: one adopt op per card, only when a newest snapshot's tag changes",
}


# Readings on which this model takes modsave_model.c's choice (READINGS).
_ALIGNABLE = frozenset(("R9", "R10", "R14", "R15"))
_OWN = os.environ.get("MODSAVE_INDEP_OWN", "")
ALIGNED = frozenset() if _OWN == "1" else _ALIGNABLE - frozenset(_OWN.split(","))


def own(r):
    return r not in ALIGNED


def tset(t, i, v):
    return t[:i] + (v,) + t[i + 1:]


def is_tick(ev):
    return ev[0] in TICKS


def legacy_of(c):
    return c == "P"


def nonempty(c):
    return c != "E"


class State:
    __slots__ = ("cfg", "blk", "spec", "ss", "rw", "loaded", "entered", "enabled", "schema",
                 "armed", "mounted", "s_card", "s_disk", "dirty", "sc_mem", "sc_disk",
                 "sc_fault", "modset", "queue", "next_root")

    def copy(self):
        t = State.__new__(State)
        for k in State.__slots__:
            setattr(t, k, getattr(self, k))
        return t


def _boot_block(s, gen=0, arena=None):
    n = s.cfg.n_mods
    return Block(UNKNOWN, gen, False, (None,) * n, None,
                 (None,) * s.cfg.n_slots, arena if arena is not None else (0,) * n)


def initial(cfg, loaded):
    s = State.__new__(State)
    s.cfg = cfg
    s.next_root = 0
    lm = tuple(m in loaded for m in range(cfg.n_mods))
    s.loaded = s.entered = s.enabled = lm
    s.schema = (1,) * cfg.n_mods
    s.armed = any(lm)
    s.mounted = (False,) * cfg.n_slots
    blank = ("E",) * cfg.n_idx
    s.s_card = (blank,) * cfg.n_slots
    s.s_disk = (blank,) * cfg.n_slots
    s.dirty = (False,) * cfg.n_slots
    s.sc_mem = (None,) * cfg.n_slots
    s.sc_disk = (None,) * cfg.n_slots
    s.sc_fault = False
    s.modset = (0,) * cfg.n_slots
    s.queue = ()
    s.spec = s.ss = s.rw = None
    s.blk = _boot_block(s)
    return s


def active(s, m):
    return s.entered[m] and s.enabled[m]


def session_tag(s):
    return sum(1 << m for m in range(s.cfg.n_mods) if s.entered[m])


def current_identity(b):
    if isinstance(b.key, tuple):
        return (b.key[2], b.gen)
    if b.key == CLEARED:
        return (CLEARED, b.gen)
    return None


def _take_fresh(s):
    s.next_root += 1
    return (s.next_root, 0)


def _log(s, fx, kind):
    if s.spec is None:
        fx.append("L:" + kind)


# ── sidecar tables ────────────────────────────────────────────────────────

def _empty_table(s):
    return ((),) * s.cfg.n_idx


def _apply_op(s, table, op, fx, prefix):
    """Apply one sidecar op to a table; return the new table. Emits tokens when fx is a list."""
    kind, slot, idx = op[0], op[1], op[2]
    if table is None:
        table = _empty_table(s)

    def tok(name):
        if fx is not None:
            fx.append(f"{prefix}{slot}.{idx}:{name}")
    if kind == "seed":
        table = tset(table, idx, (op[3],) + table[idx])
        tok("seed")
    elif kind == "drop":
        tok("drop")
        table = tset(table, idx, ())
    elif kind == "format":
        tok("format")
        table = _empty_table(s)
    elif kind == "adopt":
        # one op per card: the newest snapshot of every index op[4] names
        for i in op[4]:
            snaps = table[i]
            table = tset(table, i, (snaps[0]._replace(tag=op[3]),) + snaps[1:])
        tok("adopt")
    elif kind == "snap":
        sn = op[3]
        snaps = list(table[idx])
        hit = [i for i, x in enumerate(snaps) if x.content == sn.content and x.lin == sn.lin]
        if hit:
            del snaps[hit[0]]
            snaps.insert(0, sn)
            tok("front")
        else:
            snaps.insert(0, sn)
            tok("put")
            if len(snaps) > s.cfg.cap:
                snaps.pop()
                tok("evict")
        table = tset(table, idx, tuple(snaps))
    return table


def view_table(s, slot):
    """The in-memory sidecar with the speculative queue applied (the lookup view)."""
    t = s.sc_mem[slot]
    for op in s.queue:
        if op[1] == slot:
            t = _apply_op(s, t, op, None, "")
    return t


def view_snaps(s, slot, idx):
    t = view_table(s, slot)
    return () if t is None else t[idx]


def find_source(s, slot, idx, h, lin):
    snaps = view_snaps(s, slot, idx)
    for sn in snaps:
        if sn.content == h and sn.lin == lin:
            return sn, True
    for sn in snaps:
        if sn.lin == lin:
            return sn, False
    return None, False


def _sidecar_write(s, fx, slot):
    if s.sc_fault:
        s.sc_fault = False
        _log(s, fx, "sidecar-write-failed")
        return
    s.sc_disk = tset(s.sc_disk, slot, s.sc_mem[slot])


def _commit_ops(s, fx, ops):
    slots = []
    for op in ops:
        s.sc_mem = tset(s.sc_mem, op[1], _apply_op(s, s.sc_mem[op[1]], op, fx, "S"))
        if op[1] not in slots:
            slots.append(op[1])
    for slot in slots:
        _sidecar_write(s, fx, slot)


def _emit(s, fx, ops):
    if not ops:
        return
    if s.spec is not None:
        for op in ops:
            if op[0] != "seed-empty":
                fx.append(f"Q{op[1]}.{op[2]}:{op[0]}")
        s.queue = s.queue + tuple(ops)
    else:
        _commit_ops(s, fx, ops)


def _seed_lineage(s):
    return _take_fresh(s)


def _seed_ops(s, slot, written):
    if view_table(s, slot) is not None:
        return []
    ops = []
    tag = s.modset[slot] - 1 if s.modset[slot] else 0
    for idx in range(s.cfg.n_idx):
        if idx not in written and nonempty(s.s_card[slot][idx]):
            ops.append(("seed", slot, idx,
                        Snap(s.s_card[slot][idx], _seed_lineage(s), tag, (None,) * s.cfg.n_mods)))
    if not ops:
        ops.append(("seed-empty", slot, 0))
    return ops


# ── card image ────────────────────────────────────────────────────────────

def _card_write(s, slot, updates, flush_ok=True):
    row = list(s.s_card[slot])
    for idx, c in updates.items():
        row[idx] = c
    s.s_card = tset(s.s_card, slot, tuple(row))
    if s.spec is not None:
        s.dirty = tset(s.dirty, slot, True)
        return
    if not flush_ok:
        return
    s.s_disk = tset(s.s_disk, slot, s.s_card[slot])
    s.dirty = tset(s.dirty, slot, False)
    s.rw = None


# ── delivery ──────────────────────────────────────────────────────────────

def status_for(s, b, m):
    """(status, schema, payload or None) for mod m against the block's key."""
    if b.key == CLEARED:
        return ("ABSENT", 0, None)
    slot, idx, lin, h = b.key
    src, match = find_source(s, slot, idx, h, lin)
    rec = src.recs[m] if src is not None else None
    if rec is None:
        return ("LEGACY_PAD", 0, "P") if b.legacy else ("ABSENT", 0, None)
    if rec.schema > s.schema[m]:
        return ("NEWER", rec.schema, None)
    if not match:
        return ("STALE", rec.schema, rec.payload)
    return ("GAP" if rec.gap else "OK", rec.schema, rec.payload)


def _arena_after(m, status, payload):
    if status in ("OK", "GAP", "STALE"):
        return payload
    if status == "LEGACY_PAD":
        return 2 if m == 0 else 0
    return 0


def _resolve(s, fx):
    cands, legacy, content, chosen = s.blk.pload
    if not cands:
        s.blk = s.blk._replace(key=CLEARED, gen=s.blk.gen + 1, legacy=False, pload=None)
        _log(s, fx, "load-no-candidate")
        return
    if len(cands) == 1:
        slot, idx = next(iter(cands))
    elif chosen in cands:
        slot, idx = chosen
    else:
        slot, idx = sorted(cands)[0]
        _log(s, fx, "multimatch-lowest")
    snaps = view_snaps(s, slot, idx)
    match = [sn for sn in snaps if sn.content == content]
    if match:
        lin = match[0].lin
    elif snaps:
        lin = snaps[0].lin
    else:
        lin = _take_fresh(s)
    s.blk = s.blk._replace(key=(slot, idx, lin, content), gen=s.blk.gen + 1, legacy=legacy,
                           pload=None)
    src, _ = find_source(s, slot, idx, content, lin)
    if src is not None and src.tag != session_tag(s):
        _log(s, fx, "mismatch")


def deliver(s, fx):
    if not s.armed:
        return
    if s.blk.pload is not None:
        _resolve(s, fx)
    cur = current_identity(s.blk)
    if cur is None:
        return
    for m in range(s.cfg.n_mods):
        if not active(s, m) or s.blk.held[m] == cur:
            continue
        status, schema, pay = status_for(s, s.blk, m)
        fx.append(f"R{m}:{status}:{schema}:{'-' if pay is None else pay}")
        s.blk = s.blk._replace(held=tset(s.blk.held, m, cur),
                               arena=tset(s.blk.arena, m, _arena_after(m, status, pay)))


# ── write capture ─────────────────────────────────────────────────────────

def _issue(s, fx, slot, idx, content, fails):
    b = s.blk
    if not s.armed:
        return Entry(False, slot, idx, content, b.gen, None, False, None, 0)
    if not nonempty(content):
        return Entry(True, slot, idx, content, b.gen, None, False, None, 0)
    key = b.key
    src = None
    if isinstance(key, tuple):
        lin, fresh = key[2], False
        src, match = find_source(s, key[0], key[1], key[3], key[2])
        if src is not None and not match:
            _log(s, fx, "carry-from-newest")
    else:
        if key == UNKNOWN:
            _log(s, fx, "write-key-unknown")
        lin, fresh = _take_fresh(s), True
    recs = []
    for m in range(s.cfg.n_mods):
        prev = src.recs[m] if src is not None else None
        if active(s, m):
            if prev is not None and prev.schema > s.schema[m]:
                _log(s, fx, "refuse-newer")
                recs.append(prev)
                continue
            fx.append(f"W{m}")
            if m in fails:
                recs.append(prev)
            else:
                recs.append(Rec(s.schema[m], 0, s.blk.arena[m]))
        else:
            recs.append(prev._replace(gap=1) if prev is not None else None)
    en = tuple(m for m in range(s.cfg.n_mods) if active(s, m))
    return Entry(True, slot, idx, content, s.blk.gen, lin, fresh, tuple(recs), session_tag(s), en)


def _complete_ok(s, fx, e):
    if not e.captured:
        return
    ops = _seed_ops(s, e.slot, (e.idx,))
    if not nonempty(e.content):
        ops.append(("drop", e.slot, e.idx))
    else:
        h = s.s_card[e.slot][e.idx]
        ops.append(("snap", e.slot, e.idx, Snap(h, e.lin, e.tag, e.recs)))
        if e.gen == s.blk.gen:
            s.blk = s.blk._replace(key=(e.slot, e.idx, e.lin, h), legacy=legacy_of(h))
            cur = current_identity(s.blk)
            held = tuple(cur if m in e.en else s.blk.held[m] for m in range(s.cfg.n_mods))
            s.blk = s.blk._replace(held=held)
    _emit(s, fx, ops)


# ── events ────────────────────────────────────────────────────────────────

def ev_T(s, fx, ev):
    deliver(s, fx)


def ev_load(s, fx, ev):
    deliver(s, fx)
    if not s.armed:
        return
    slot, idx = ev[1], ev[2]
    c = s.s_card[slot][idx]
    if c == "T":
        return
    cands = frozenset((p, i) for p in range(s.cfg.n_slots) if s.mounted[p]
                      for i in range(s.cfg.n_idx) if s.s_card[p][i] == c)
    chosen = (slot, idx) if ev[0] == "LOAD" else None
    s.blk = s.blk._replace(pload=(cands, legacy_of(c), c, chosen))


def ev_reset(s, fx, ev):
    deliver(s, fx)
    if s.armed:
        s.blk = s.blk._replace(key=CLEARED, gen=s.blk.gen + 1, legacy=False, pload=None)


def ev_write(s, fx, ev):
    deliver(s, fx)
    slot, idx, content = ev[1], ev[2], ev[3]
    fails = ev[4] if len(ev) > 4 else ()
    e = _issue(s, fx, slot, idx, content, fails)
    if ev[0] == "WRITE_FAIL":
        _card_write(s, slot, {idx: content}, flush_ok=False)
        _log(s, fx, "flush-failed")
        return
    _card_write(s, slot, {idx: content})
    _complete_ok(s, fx, e)


def ev_issue(s, fx, ev):
    deliver(s, fx)
    e = _issue(s, fx, ev[1], ev[2], ev[3], ev[4])
    s.blk = s.blk._replace(entries=tset(s.blk.entries, ev[1], e))


def ev_complete(s, fx, ev):
    deliver(s, fx)
    slot, outcome = ev[1], ev[2]
    e = s.blk.entries[slot]
    s.blk = s.blk._replace(entries=tset(s.blk.entries, slot, None))
    if outcome == "ok":
        _card_write(s, slot, {e.idx: e.content})
        _complete_ok(s, fx, e)
    elif outcome == "torn":
        _card_write(s, slot, {e.idx: "T"})
        if e.captured and nonempty(e.content):
            _log(s, fx, "completion-checksum-failed")


def ev_file_create(s, fx, ev):
    deliver(s, fx)
    slot = ev[1]
    # one write over every record: one entry per record, completed in order
    for i in range(s.cfg.n_idx):
        _card_write(s, slot, {i: "E"})
        if s.armed:
            _emit(s, fx, _seed_ops(s, slot, (i,)) + [("drop", slot, i)])


def ev_format(s, fx, ev):
    deliver(s, fx)
    slot = ev[1]
    every = tuple(range(s.cfg.n_idx))
    _card_write(s, slot, {i: "E" for i in every})
    if s.armed:
        _emit(s, fx, _seed_ops(s, slot, every) + [("format", slot, 0)])


def ev_play(s, fx, ev):
    deliver(s, fx)
    s.blk = s.blk._replace(arena=tset(s.blk.arena, ev[1], ev[2]))


def ev_toggle(s, fx, ev):
    s.enabled = tset(s.enabled, ev[1], not s.enabled[ev[1]])


def ev_enter(s, fx, ev):
    s.entered = tset(s.entered, ev[1], True)
    if own("R14"):
        s.blk = s.blk._replace(arena=tset(s.blk.arena, ev[1], 0))


def ev_schema(s, fx, ev):
    s.schema = tset(s.schema, ev[1], 3 - s.schema[ev[1]])


def _rebind(s, fx):
    """Port rebind from the state's card section (R11): the model's cards
    never change ports, so the key's slot stands."""
    return


def _new_process(s, loaded, entered):
    n = s.cfg.n_mods
    s.loaded = tuple(m in loaded for m in range(n))
    s.entered = tuple(m in entered for m in range(n))
    s.enabled = s.loaded
    s.armed = any(s.loaded)
    s.mounted = (False,) * s.cfg.n_slots
    s.s_card = (("E",) * s.cfg.n_idx,) * s.cfg.n_slots   # no image read before first access
    s.dirty = (False,) * s.cfg.n_slots
    s.sc_mem = (None,) * s.cfg.n_slots
    s.sc_fault = False
    s.queue = ()
    s.spec = s.rw = None


def ev_restart(s, fx, ev):
    _new_process(s, ev[1], ev[2])
    s.blk = _boot_block(s)


def ev_handoff(s, fx, ev):
    old_entered = s.entered
    _new_process(s, ev[1], ev[2])
    arena = tuple(s.blk.arena[m] if not own("R10") or (old_entered[m] and s.entered[m]) else 0
                  for m in range(s.cfg.n_mods))
    s.blk = s.blk._replace(arena=arena)
    if s.armed:
        _rebind(s, fx)
    s.ss = s.blk


def _mount(s, fx, slot):
    s.mounted = tset(s.mounted, slot, True)
    s.s_card = tset(s.s_card, slot, s.s_disk[slot])
    d = s.sc_disk[slot]
    if d == CORRUPT:
        _log(s, fx, "sidecar-unparseable")
        s.sc_disk = tset(s.sc_disk, slot, None)
        d = None
    s.sc_mem = tset(s.sc_mem, slot, d)
    if d is not None and not s.armed:
        s.armed = True
        _rebind(s, fx)


def ev_mount(s, fx, ev):
    _mount(s, fx, ev[1])
    if ev[0] == "MOUNT_ADOPT" and s.armed and s.sc_mem[ev[1]] is not None:
        slot = ev[1]
        tag = session_tag(s)
        idxs = tuple(i for i in range(s.cfg.n_idx) if nonempty(s.s_card[slot][i])
                     and s.sc_mem[slot][i] and s.sc_mem[slot][i][0].tag != tag)
        if idxs:
            _commit_ops(s, fx, [("adopt", slot, 0, tag, idxs)])


def ev_ss_take(s, fx, ev):
    s.ss = s.blk


def ev_ss_load(s, fx, ev):
    s.blk = s.ss
    if not own("R15"):
        s.rw = None
    if s.armed:
        _rebind(s, fx)


def ev_ss_load_old(s, fx, ev):
    s.blk = _boot_block(s, gen=s.blk.gen, arena=s.ss.arena)
    if not own("R15"):
        s.rw = None


def ev_rw_mark(s, fx, ev):
    s.rw = s.blk


def ev_rewind(s, fx, ev):
    s.blk = s.rw


def ev_ra_begin(s, fx, ev):
    s.spec = s.blk


def _restore_dirty(s):
    s.s_card = tuple(s.s_disk[p] if s.dirty[p] else s.s_card[p] for p in range(s.cfg.n_slots))
    s.dirty = (False,) * s.cfg.n_slots


def ev_ra_rollback(s, fx, ev):
    s.blk = s.spec
    s.spec = None
    s.queue = ()
    _restore_dirty(s)


def ev_ra_adopt(s, fx, ev):
    s.spec = None
    if any(s.dirty):
        s.s_disk = tuple(s.s_card[p] if s.dirty[p] else s.s_disk[p] for p in range(s.cfg.n_slots))
        s.dirty = (False,) * s.cfg.n_slots
        s.rw = None
    q, s.queue = s.queue, ()
    _commit_ops(s, fx, list(q))


def ev_ra_adopt_fail(s, fx, ev):
    s.spec = None
    failed = any(s.dirty) or bool(s.queue)
    s.queue = ()
    if own("R9"):
        if any(s.dirty):
            _log(s, fx, "adoption-flush-failed")
        return
    s.dirty = (False,) * s.cfg.n_slots
    if failed:
        _log(s, fx, "adoption-flush-failed")


def ev_ra_partial(s, fx, ev):
    s.spec = None
    s.queue = ()
    _restore_dirty(s)


def ev_edit(s, fx, ev):
    slot, idx, c = ev[1], ev[2], ev[3]
    s.s_disk = tset(s.s_disk, slot, tset(s.s_disk[slot], idx, c))


def ev_sc_corrupt(s, fx, ev):
    s.sc_disk = tset(s.sc_disk, ev[1], CORRUPT)


def ev_sc_fault(s, fx, ev):
    s.sc_fault = True


def ev_modset(s, fx, ev):
    s.modset = tset(s.modset, ev[1], 0 if ev[2] is None else 1 + sum(1 << m for m in ev[2]))


HANDLERS = {
    "T": ev_T, "LOAD": ev_load, "LOAD_STALE": ev_load, "RESET": ev_reset, "WRITE": ev_write,
    "WRITE_FAIL": ev_write, "ISSUE": ev_issue, "COMPLETE": ev_complete,
    "FILE_CREATE": ev_file_create, "FORMAT": ev_format, "PLAY": ev_play, "TOGGLE": ev_toggle,
    "ENTER": ev_enter, "SCHEMA": ev_schema, "RESTART": ev_restart, "MOUNT": ev_mount,
    "MOUNT_ADOPT": ev_mount, "SS_TAKE": ev_ss_take, "SS_LOAD": ev_ss_load,
    "SS_LOAD_OLD": ev_ss_load_old, "RA_BEGIN": ev_ra_begin, "RA_ROLLBACK": ev_ra_rollback,
    "RA_ADOPT": ev_ra_adopt, "RA_ADOPT_FAIL": ev_ra_adopt_fail, "RA_PARTIAL": ev_ra_partial,
    "RW_MARK": ev_rw_mark, "REWIND": ev_rewind, "EDIT": ev_edit, "SC_CORRUPT": ev_sc_corrupt,
    "SC_FAULT": ev_sc_fault, "HANDOFF": ev_handoff, "MODSET": ev_modset,
}


def enabled(s, ev):
    k = ev[0]
    if s.spec is not None:
        if k not in TICKS and k not in SPEC_HOST:
            return False
    elif k in SPEC_HOST:
        return False
    if k in ("LOAD", "LOAD_STALE"):
        return s.mounted[ev[1]] and nonempty(s.s_card[ev[1]][ev[2]])
    if k == "ISSUE":
        return s.mounted[ev[1]] and s.blk.entries[ev[1]] is None
    if k in ("WRITE", "WRITE_FAIL", "FILE_CREATE", "FORMAT"):
        return s.mounted[ev[1]]
    if k == "SC_CORRUPT":
        return s.sc_disk[ev[1]] not in (None, CORRUPT)
    if k == "MODSET":
        return s.modset[ev[1]] != (0 if ev[2] is None else 1 + sum(1 << m for m in ev[2]))
    if k == "COMPLETE":
        return s.blk.entries[ev[1]] is not None
    if k == "PLAY":
        return active(s, ev[1]) and s.blk.arena[ev[1]] != ev[2]
    if k == "EDIT":
        return s.s_disk[ev[1]][ev[2]] != ev[3]
    if k in ("TOGGLE", "SCHEMA"):
        return s.entered[ev[1]]
    if k == "ENTER":
        return s.loaded[ev[1]] and not s.entered[ev[1]]
    if k in ("RESTART", "HANDOFF"):
        return set(ev[2]) <= set(ev[1])
    if k in ("MOUNT", "MOUNT_ADOPT"):
        return not s.mounted[ev[1]]
    if k in ("SS_LOAD", "SS_LOAD_OLD"):
        return s.ss is not None
    if k == "REWIND":
        return s.rw is not None
    if k == "RA_BEGIN":
        return s.spec is None
    if k == "RA_ADOPT_FAIL" and own("R9"):
        return any(s.dirty)
    if k == "SC_FAULT":
        return not s.sc_fault
    return True


def apply(s, ev):
    t = s.copy()
    fx = []
    HANDLERS[ev[0]](t, fx, ev)
    return t, fx


# ── canonical form ────────────────────────────────────────────────────────

def canon(s):
    """Hashable canonical form: lineage roots renamed by first occurrence and
    each root's counters taken relative to the least one the state holds;
    generations kept only as equality with their block's generation."""
    roots = {}

    def first(lin):
        if lin is not None and lin != CLEARED:
            roots.setdefault(lin[0], len(roots))
        return None
    _canon(s, first)
    low = {}
    for lin in _lins(s):
        low[lin[0]] = min(low.get(lin[0], lin[1]), lin[1])
    return _canon(s, lambda lin: lin if lin in (None, CLEARED) else
                  (roots[lin[0]], lin[1] - low[lin[0]]))


def _lins(s):
    out = []

    def snap(sn):
        out.append(sn.lin)

    for b in (s.blk, s.spec, s.ss, s.rw):
        if b is None:
            continue
        if isinstance(b.key, tuple):
            out.append(b.key[2])
        out += [h[0] for h in b.held if h is not None and h[0] != CLEARED]
        out += [e.lin for e in b.entries if e is not None and e.lin is not None]
    for t in s.sc_mem + s.sc_disk:
        if t not in (None, CORRUPT):
            for snaps in t:
                for sn in snaps:
                    snap(sn)
    for o in s.queue:
        if o[0] in ("snap", "seed"):
            snap(o[3])
    return out


def _canon(s, L):
    def snap(sn):
        return (sn.content, L(sn.lin), sn.tag, sn.recs)

    def table(t):
        if t in (None, CORRUPT):
            return t
        return tuple(tuple(snap(x) for x in snaps) for snaps in t)

    def block(b):
        if b is None:
            return None
        key = b.key if not isinstance(b.key, tuple) else (b.key[0], b.key[1], L(b.key[2]),
                                                            b.key[3])
        held = tuple(None if h is None else (L(h[0]), h[1] == b.gen) for h in b.held)
        ents = tuple(None if e is None else
                     (e.captured, e.slot, e.idx, e.content, e.gen == b.gen, L(e.lin), e.fresh,
                      e.recs, e.tag, e.en) for e in b.entries)
        return (key, b.legacy, held, b.pload, ents, b.arena)

    def op(o):
        if o[0] in ("snap", "seed"):
            return o[:3] + (snap(o[3]),)
        return o
    return (block(s.blk), block(s.spec), block(s.ss), block(s.rw), s.loaded, s.entered,
            s.enabled, s.schema, s.armed, s.mounted, s.s_card, s.s_disk, s.dirty,
            tuple(table(t) for t in s.sc_mem), tuple(table(t) for t in s.sc_disk), s.sc_fault,
            s.modset, tuple(op(o) for o in s.queue))


OPEN_QUESTIONS = {k: ("[aligned] " if k in ALIGNED else "") + v for k, v in READINGS.items()}
