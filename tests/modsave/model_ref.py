"""Reference model of the per-mod save-record rules, written as a rule table.

This is the Python side of the save-record state-model test: an
independent statement of the rules in the plan's normative "Block and
events" table, the Records status precedence, the write capture, the
sidecar and the arming rule, over a finite domain. The C module
tests/modsave/modsave_model.c states the same rules in structs; the
search (state_search.py) applies every event to both and requires equal
effects on every edge.

Domain (Config): N_MODS mods, N_SLOTS card slots, N_IDX save indices,
CAP snapshots per index. Record contents are single letters: 'E' empty,
'X' and 'Y' with a zero legacy span, 'P' with a non-zero legacy span that
carries mod 0's magic (foreign to every other mod), 'T' a torn record
that fails the checksum verdict. Arena values are 0 (the fresh
playthrough), 1 and 2; a LEGACY_PAD read of 'P' imports value 2 for mod
0 and nothing for the others.

State: `blk` is the key's block plus the grow-only entry section and
every mod's arena value, the state that savestates, rewind and runahead
roll back; everything else is host state.

Effects are strings in a fixed grammar shared with the C module:
  R<m>:<STATUS>:<schema>:<payload>   read callback (payload '-' = no data)
  W<m>                               write callback called
  S<slot>.<idx>:<op>                 sidecar snapshot mutation committed
                                     (put, front, drop, evict, seed, format)
  Q<slot>.<idx>:<op>                 the same op queued on a speculative tick
  L:<kind>                           log line (non-speculative ticks only)
Lineage bytes never appear in effects.

Interpretations of open points are marked "Q<n>" and listed in
OPEN_QUESTIONS; the C module takes the same reading of each.
"""

from dataclasses import dataclass, field, replace
from typing import Optional, Tuple

STATUSES = ("OK", "ABSENT", "GAP", "STALE", "NEWER", "LEGACY_PAD")

OPEN_QUESTIONS = {
    "Q1": "Closed (V45): a fresh lineage is minted from entropy where it is "
          "consumed and held in the block or the write's entry; a cleared "
          "key's identity is (cleared, generation), so an issue changes no "
          "identity.",
    "Q2": "Closed (V47): a new-playthrough reset drops a pending load.",
    "Q3": "Closed (V47): one candidate resolves to itself; among several, the "
          "game's selection when it names one, else the lowest (slot, index), "
          "logged (V51); no candidate is a new-playthrough reset.",
    "Q4": "Closed: seeding takes a fresh entropy root per non-empty index "
          "except the one being written; at a format the seed reads the "
          "already formatted image, so it creates an empty .modsave.",
    "Q5": "Closed: the port rebind reads the state's card section; the "
          "model's cards never change ports, so the key's slot stands.",
    "Q6": "Mismatch log: emitted only when the resolved source snapshot's tag "
          "differs from the session tag; no log when there is no snapshot.",
    "Q7": "Failed adoption flush: the dirty marks are cleared and s_card keeps "
          "the led writes that never reached disk.",
    "Q8": "A failed card flush on a committed tick leaves the record in s_card "
          "(openpete_memcard_write copies before flushing) and queues nothing.",
    "Q9": "Closed (V45): the write call is the unit, one entry per port; file "
          "create is one write of empty records, modelled synchronously.",
    "Q10": "Rewind: under the 2026-10-11 rule a committed card write clears the "
           "rewind ring and keyframes carry no card, so a rewind never crosses "
           "a committed save and replay dedupe is unreachable; the model's "
           "rewind restores the block only.",
    "Q11": "Savestate card image: the normative table gives a savestate load no "
           "card effect, and the code carries no card image (a load is refused "
           "while no port holds the state's card); the model follows the table.",
}


@dataclass(frozen=True)
class Config:
    n_mods: int = 2
    n_slots: int = 2
    n_idx: int = 2
    cap: int = 4


@dataclass(frozen=True)
class Rec:
    schema: int
    gap: bool
    payload: int


@dataclass(frozen=True)
class Snap:
    content: str
    lin: Tuple[int, int]
    tag: Tuple[int, ...]          # entered mods at the write (session tag)
    recs: Tuple[Optional[Rec], ...]


@dataclass(frozen=True)
class Entry:
    slot: int
    idx: int
    gen: int
    content: str
    lin: Optional[Tuple[int, int]]   # None for an empty-record entry
    recs: Tuple[Optional[Rec], ...]
    tag: Tuple[int, ...]
    captured: bool = True            # False: issued while unarmed, card write only
    fresh: bool = False              # the lineage was minted at this issue
    en: Tuple[int, ...] = ()         # mods enabled at issue (V45)
    norec: bool = False              # the write covers no record: its completion has no effect
    orphan: bool = False             # the BIOS write of a blob that predates the entries


@dataclass(frozen=True)
class Block:
    key: object                # None | 'C' | (slot, idx, lin, content)
    gen: int
    legacy: bool               # the legacy copy is non-zero
    held: Tuple[object, ...]   # per mod: None | (lin, gen)
    pload: object              # None | (cands, legacy, content, chosen)
    entries: Tuple[Optional[Entry], ...]   # per slot
    arena: Tuple[int, ...]


@dataclass(frozen=True)
class Mod:
    loaded: bool
    entered: bool
    enabled: bool
    schema: int


@dataclass
class State:
    cfg: Config
    blk: Block
    mods: Tuple[Mod, ...]
    armed: bool
    mounted: Tuple[bool, ...]
    s_card: Tuple[Tuple[str, ...], ...]
    s_disk: Tuple[Tuple[str, ...], ...]
    dirty: Tuple[bool, ...]
    sc_mem: Tuple[object, ...]    # per slot: None | tuple per idx of tuple of Snap
    sc_disk: Tuple[object, ...]   # per slot: None | 'CORRUPT' | table
    sc_fault: bool
    modset: Tuple[object, ...]    # per slot: None | tag (legacy card-level .modset)
    queue: Tuple[tuple, ...]
    spec: Optional[Block]
    ss: Optional[Block]
    rw: Optional[Block]
    next_root: int
    effects: list = field(default_factory=list)


# ── helpers ────────────────────────────────────────────────────────────────

def tset(t, i, v):
    return t[:i] + (v,) + t[i + 1:]


def pad_nonzero(content):
    return content == "P"


def new_root(s):
    r = s.next_root
    s.next_root += 1
    return (r, 0)


def boot_block(s, cfg):
    return Block(key=None, gen=0, legacy=False,
                 held=(None,) * cfg.n_mods, pload=None,
                 entries=(None,) * cfg.n_slots, arena=(0,) * cfg.n_mods)


def initial(cfg=Config(), loaded=None):
    """Process boot with every listed mod loaded, entered and enabled; blank cards."""
    loaded = tuple(range(cfg.n_mods)) if loaded is None else loaded
    s = State(cfg=cfg, blk=None,
              mods=tuple(Mod(i in loaded, i in loaded, True, 1) for i in range(cfg.n_mods)),
              armed=bool(loaded), mounted=(False,) * cfg.n_slots,
              s_card=(("E",) * cfg.n_idx,) * cfg.n_slots,
              s_disk=(("E",) * cfg.n_idx,) * cfg.n_slots,
              dirty=(False,) * cfg.n_slots, sc_mem=(None,) * cfg.n_slots,
              sc_disk=(None,) * cfg.n_slots, sc_fault=False,
              modset=(None,) * cfg.n_slots, queue=(), spec=None, ss=None, rw=None,
              next_root=0)
    s.blk = boot_block(s, cfg)
    return s


def clone(s):
    c = State(**{k: getattr(s, k) for k in s.__dataclass_fields__ if k != "effects"})
    c.effects = []
    return c


def active(s, m):
    md = s.mods[m]
    return md.loaded and md.entered and md.enabled


def registered(s, m):
    md = s.mods[m]
    return md.loaded and md.entered


def session_tag(s):
    return tuple(m for m in range(s.cfg.n_mods) if registered(s, m))


def speculative(s):
    return s.spec is not None


def log(s, kind):
    if not speculative(s):
        s.effects.append("L:" + kind)


def current_identity(b):
    if b.key is None:
        return None
    if b.key == "C":
        return (None, b.gen)
    return (b.key[2], b.gen)


# ── sidecar view and host ops ──────────────────────────────────────────────

def apply_op(cfg, table, op, emit=None):
    """Apply one host op to a sidecar table (tuple per index of snapshot tuples)."""
    kind = op[0]
    if kind == "format":
        if emit is not None:
            emit(f"S{op[1]}.0:format")
        return ((),) * cfg.n_idx
    _, slot, idx = op[:3]
    snaps = table[idx]
    if kind == "drop":
        if emit is not None:
            emit(f"S{slot}.{idx}:drop")
        return tset(table, idx, ())
    if kind == "seed":
        snap = op[3]
        if emit is not None:
            emit(f"S{slot}.{idx}:seed")
        return tset(table, idx, (snap,) + snaps)
    snap = op[3]
    same = [k for k, x in enumerate(snaps) if x.content == snap.content and x.lin == snap.lin]
    if same:
        rest = snaps[:same[0]] + snaps[same[0] + 1:]
        if emit is not None:
            emit(f"S{slot}.{idx}:front")
        return tset(table, idx, (snap,) + rest)
    new = (snap,) + snaps
    if emit is not None:
        emit(f"S{slot}.{idx}:put")
    if len(new) > cfg.cap:
        new = new[:cfg.cap]
        if emit is not None:
            emit(f"S{slot}.{idx}:evict")
    return tset(table, idx, new)


def view(s, slot):
    """The slot's sidecar as lookups see it: memory plus the speculative queue."""
    t = s.sc_mem[slot]
    if t is None:
        t = ((),) * s.cfg.n_idx
    for op in s.queue:
        if op[0] == "format" and op[1] == slot:
            t = apply_op(s.cfg, t, op)
        elif op[0] != "format" and op[1] == slot:
            t = apply_op(s.cfg, t, op)
    return t


def seed_card(s, slot, skip_idx):
    """First .modsave of a card (Q4): one seeded snapshot per non-empty index."""
    t = ((),) * s.cfg.n_idx
    tag = s.modset[slot] if s.modset[slot] is not None else ()
    for i in range(s.cfg.n_idx):
        c = s.s_card[slot][i]
        if i == skip_idx or c in ("E", "T"):
            continue
        snap = Snap(c, new_root(s), tag, (None,) * s.cfg.n_mods)
        s.effects.append(f"S{slot}.{i}:seed")
        t = tset(t, i, (snap,))
    s.sc_mem = tset(s.sc_mem, slot, t)


def sidecar_write(s, slot):
    if s.sc_fault:
        s.sc_fault = False
        log(s, "sidecar-write-failed")
        return
    s.sc_disk = tset(s.sc_disk, slot, s.sc_mem[slot])


def commit_op(s, op):
    slot = op[1]
    if s.sc_mem[slot] is None:
        seed_card(s, slot, op[2] if op[0] != "format" else -1)
    s.sc_mem = tset(s.sc_mem, slot, apply_op(s.cfg, s.sc_mem[slot], op, s.effects.append))
    sidecar_write(s, slot)


def host_op(s, op):
    if speculative(s):
        s.queue = s.queue + (op,)
        where = f"{op[1]}.{op[2] if op[0] != 'format' else 0}"
        s.effects.append(f"Q{where}:{op[0]}")
    else:
        commit_op(s, op)


def flush_card(s, slot):
    """The card flush after a write: committed ticks write the image, led ticks mark it dirty."""
    if speculative(s):
        s.dirty = tset(s.dirty, slot, True)
    else:
        s.s_disk = tset(s.s_disk, slot, s.s_card[slot])
        s.rw = None   # a committed card write clears the rewind ring (Q10)


# ── status and delivery ────────────────────────────────────────────────────

def source(s, b):
    slot, idx, lin, content = b.key
    snaps = view(s, slot)[idx]
    for x in snaps:
        if x.content == content and x.lin == lin:
            return ("match", x)
    for x in snaps:
        if x.lin == lin:
            return ("newest", x)
    return None


def status_for(s, b, m):
    if b.key == "C":
        return ("LEGACY_PAD", 0, "P") if b.legacy else ("ABSENT", 0, None)
    src = source(s, b)
    rec = src[1].recs[m] if src else None
    if rec is None:
        return ("LEGACY_PAD", 0, "P") if b.legacy else ("ABSENT", 0, None)
    if rec.schema > s.mods[m].schema:
        return ("NEWER", rec.schema, None)
    if src[0] == "newest":
        return ("STALE", rec.schema, rec.payload)
    return ("GAP" if rec.gap else "OK", rec.schema, rec.payload)


def mod_reacts(m, status, data):
    """The fixture's read callback: the arena value the read leaves."""
    if status in ("OK", "GAP", "STALE"):
        return data
    if status == "LEGACY_PAD" and m == 0 and data == "P":
        return 2
    return 0


def resolve_load(s):
    b = s.blk
    cands, legacy, content, chosen = b.pload
    if not cands:
        s.blk = replace(b, key="C", gen=b.gen + 1, legacy=False, pload=None)
        log(s, "load-no-candidate")
        return
    if len(cands) == 1:
        pick = next(iter(cands))
    elif chosen in cands:
        pick = chosen
    else:
        pick = min(cands)
        log(s, "multimatch-lowest")
    slot, idx = pick
    snaps = view(s, slot)[idx]
    match = next((x for x in snaps if x.content == content), None)
    if match is not None:
        lin = match.lin
    elif snaps:
        lin = snaps[0].lin
    else:
        lin = new_root(s)
    src_tag = match.tag if match is not None else (snaps[0].tag if snaps else None)
    if src_tag is not None and src_tag != session_tag(s):
        log(s, "mismatch")
    s.blk = replace(b, key=(slot, idx, lin, content), gen=b.gen + 1,
                    legacy=legacy, pload=None)


def deliver(s):
    if not s.armed:
        return
    if s.blk.pload is not None:
        resolve_load(s)
    b = s.blk
    cur = current_identity(b)
    if cur is None:
        return
    held, arena = list(b.held), list(b.arena)
    for m in range(s.cfg.n_mods):
        if not active(s, m) or held[m] == cur:
            continue
        st, schema, data = status_for(s, b, m)
        shown = "-" if data is None else str(data)
        s.effects.append(f"R{m}:{st}:{schema}:{shown}")
        arena[m] = mod_reacts(m, st, data)
        held[m] = cur
    s.blk = replace(b, held=tuple(held), arena=tuple(arena))


# ── write issue and completion ─────────────────────────────────────────────

def carry_source(s, b, slot, idx):
    snaps = view(s, slot)[idx]
    for x in snaps:
        if x.content == b.key[3] and x.lin == b.key[2]:
            return x
    for x in snaps:
        if x.lin == b.key[2]:
            log(s, "carry-from-newest")
            return x
    return None


def issue(s, slot, idx, content, fails):
    """Write issue: decide lineage, carry source and refusals, call each write."""
    b = s.blk
    if content == "E":
        s.blk = replace(b, entries=tset(b.entries, slot,
                        Entry(slot, idx, b.gen, "E", None, (), ())))
        return
    fresh = not isinstance(b.key, tuple)
    if not fresh:
        lin = b.key[2]
        src = carry_source(s, b, b.key[0], b.key[1])
    else:
        if b.key is None:
            log(s, "write-key-unknown")
        lin, src = new_root(s), None
    recs = []
    for m in range(s.cfg.n_mods):
        prev = src.recs[m] if src is not None else None
        if not active(s, m):
            recs.append(replace(prev, gap=True) if prev is not None else None)
        elif prev is not None and prev.schema > s.mods[m].schema:
            log(s, "refuse-newer")
            recs.append(prev)
        else:
            s.effects.append(f"W{m}")
            if m in fails:
                recs.append(prev)
            else:
                recs.append(Rec(s.mods[m].schema, False, b.arena[m]))
    en = tuple(m for m in range(s.cfg.n_mods) if active(s, m))
    e = Entry(slot, idx, b.gen, content, lin, tuple(recs), session_tag(s), True, fresh, en)
    s.blk = replace(b, entries=tset(b.entries, slot, e))


def complete(s, slot, outcome):
    """Write completion: 'ok', 'fail' (nothing reaches the card) or 'torn'."""
    b = s.blk
    e = b.entries[slot]
    if e is None:
        return
    s.blk = replace(b, entries=tset(b.entries, slot, None))
    if outcome == "fail":
        return
    if e.orphan:
        if not e.norec:
            s.s_card = tset(s.s_card, slot, tset(s.s_card[slot], e.idx, "T" if outcome == "torn" else e.content))
            flush_card(s, slot)
        if s.armed:
            log(s, "completion-no-entry")
        return
    if e.norec:
        return
    if outcome == "torn":
        s.s_card = tset(s.s_card, slot, tset(s.s_card[slot], e.idx, "T"))
        flush_card(s, slot)
        if e.captured and e.lin is not None:
            log(s, "completion-checksum-failed")
        return
    s.s_card = tset(s.s_card, slot, tset(s.s_card[slot], e.idx, e.content))
    flush_card(s, slot)
    if not e.captured:
        return
    if e.lin is None:
        host_op(s, ("drop", slot, e.idx))
        return
    b = s.blk
    if b.gen == e.gen:
        cur = (e.lin, b.gen)
        held = tuple(cur if m in e.en else b.held[m] for m in range(s.cfg.n_mods))
        s.blk = replace(b, key=(slot, e.idx, e.lin, e.content),
                        legacy=pad_nonzero(e.content), held=held)
    host_op(s, ("snap", slot, e.idx, Snap(e.content, e.lin, e.tag, e.recs)))


# ── the rule table: event kind -> (guard, rule) ────────────────────────────

def g_tick(s, *a):
    return True


def r_tick(s):
    deliver(s)


def g_load(s, slot, idx):
    return s.mounted[slot] and s.s_card[slot][idx] != "E"


def bind(s, slot, idx, chosen):
    if not s.armed:
        return
    content = s.s_card[slot][idx]
    if content == "T":
        return
    cands = frozenset((c, i) for c in range(s.cfg.n_slots) if s.mounted[c]
                      for i in range(s.cfg.n_idx) if s.s_card[c][i] == content)
    s.blk = replace(s.blk, pload=(cands, pad_nonzero(content), content, chosen))


def r_load(s, slot, idx):
    deliver(s)
    bind(s, slot, idx, (slot, idx))


def r_load_stale(s, slot, idx):
    deliver(s)
    bind(s, slot, idx, None)


def r_reset(s):
    deliver(s)
    if s.armed:
        b = s.blk
        s.blk = replace(b, key="C", gen=b.gen + 1, legacy=False, pload=None)


def g_write(s, slot, idx, content, fails=()):
    return s.mounted[slot] and s.blk.entries[slot] is None


def r_write(s, slot, idx, content, fails=()):
    deliver(s)
    if s.armed:
        issue(s, slot, idx, content, fails)
        complete(s, slot, "ok")
    else:
        s.s_card = tset(s.s_card, slot, tset(s.s_card[slot], idx, content))
        flush_card(s, slot)


def r_write_fail(s, slot, idx, content):
    """A committed or led write whose card flush fails (Q8)."""
    deliver(s)
    if s.armed:
        issue(s, slot, idx, content, ())
        complete(s, slot, "fail")
    s.s_card = tset(s.s_card, slot, tset(s.s_card[slot], idx, content))
    log(s, "flush-failed")


def r_issue(s, slot, idx, content, fails=()):
    deliver(s)
    if s.armed:
        issue(s, slot, idx, content, fails)
    else:
        b = s.blk
        s.blk = replace(b, entries=tset(b.entries, slot,
                        Entry(slot, idx, b.gen, content, None, (), (), False)))


def g_issue(s, slot, idx, content, fails=()):
    """One write in flight per port: no issue while the slot's entry is pending (V45)."""
    return s.mounted[slot] and s.blk.entries[slot] is None


def r_issue_norec(s, slot):
    deliver(s)
    b = s.blk
    s.blk = replace(b, entries=tset(b.entries, slot,
                    Entry(slot, 0, b.gen, "E", None, (), (), s.armed, norec=True)))


def g_abort(s, slot, idx):
    return s.mounted[slot] and s.blk.entries[slot] is None and s.s_card[slot][idx] != "E"


def r_abort(s, slot, idx):
    deliver(s)


def g_complete(s, slot, outcome):
    return s.blk.entries[slot] is not None


def r_complete(s, slot, outcome):
    deliver(s)
    complete(s, slot, outcome)


def g_mounted(s, slot):
    return s.mounted[slot] and s.blk.entries[slot] is None


def r_file_create(s, slot):
    deliver(s)
    for idx in range(s.cfg.n_idx):
        if s.armed:
            issue(s, slot, idx, "E", ())
            complete(s, slot, "ok")
        else:
            s.s_card = tset(s.s_card, slot, tset(s.s_card[slot], idx, "E"))
            flush_card(s, slot)


def r_format(s, slot):
    deliver(s)
    s.s_card = tset(s.s_card, slot, ("E",) * s.cfg.n_idx)
    flush_card(s, slot)
    if s.armed:
        host_op(s, ("format", slot))


def g_play(s, m, v):
    return active(s, m) and s.blk.arena[m] != v


def r_play(s, m, v):
    deliver(s)
    s.blk = replace(s.blk, arena=tset(s.blk.arena, m, v))


def g_toggle(s, m):
    return registered(s, m)


def r_toggle(s, m):
    md = s.mods[m]
    s.mods = tset(s.mods, m, replace(md, enabled=not md.enabled))


def g_enter(s, m):
    return s.mods[m].loaded and not s.mods[m].entered


def r_enter(s, m):
    s.mods = tset(s.mods, m, replace(s.mods[m], entered=True))


def g_schema(s, m):
    return registered(s, m)


def r_schema(s, m):
    md = s.mods[m]
    s.mods = tset(s.mods, m, replace(md, schema=3 - md.schema))


def g_quiet(s, *a):
    return s.spec is None


def r_restart(s, loaded, entered):
    cfg = s.cfg
    s.mods = tuple(replace(s.mods[m], loaded=m in loaded, entered=m in entered, enabled=True)
                   for m in range(cfg.n_mods))
    s.blk = boot_block(s, cfg)
    s.armed = bool(loaded)
    s.mounted = (False,) * cfg.n_slots
    # Cards load lazily: no image is read until the card's first access.
    s.s_card = (("E",) * cfg.n_idx,) * cfg.n_slots
    s.dirty = (False,) * cfg.n_slots
    s.sc_mem = (None,) * cfg.n_slots
    s.sc_fault = False
    s.queue = ()
    s.rw = None


def g_mount(s, slot):
    return not s.mounted[slot]


def r_mount(s, slot):
    """First card access: the card image and the sidecar are read."""
    s.mounted = tset(s.mounted, slot, True)
    s.s_card = tset(s.s_card, slot, s.s_disk[slot])
    d = s.sc_disk[slot]
    if d == "CORRUPT":
        log(s, "sidecar-unparseable")
        s.sc_disk = tset(s.sc_disk, slot, None)
        d = None
    s.sc_mem = tset(s.sc_mem, slot, d)
    if d is not None:
        s.armed = True


def r_mount_adopt(s, slot):
    """Mount under --adopt-saves: the newest snapshot of every non-empty index
    takes the session tag, and the sidecar is written."""
    r_mount(s, slot)
    t = s.sc_mem[slot]
    if t is None:
        return
    tag = session_tag(s)
    new = tuple(((replace(sn[0], tag=tag),) + sn[1:]) if sn and s.s_card[slot][i] not in ("E", "T")
                else sn for i, sn in enumerate(t))
    if new != t:
        s.sc_mem = tset(s.sc_mem, slot, new)
        s.effects.append(f"S{slot}.0:adopt")
        sidecar_write(s, slot)


def r_ss_take(s):
    s.ss = s.blk


def g_ss_load(s):
    return s.spec is None and s.ss is not None


def rebind(s):
    """Port rebind from the state's card section: the model's cards never
    change ports, so the key's slot stands."""
    return


def r_ss_load(s):
    s.blk = s.ss
    s.rw = None
    rebind(s)


def r_ss_load_old(s):
    b = s.ss
    gen = s.blk.gen
    # Guest RAM comes back with the BIOS write the state had in flight; its entry does not.
    orphans = tuple(None if e is None else
                    Entry(e.slot, e.idx, gen, e.content, None, (), (), False, norec=e.norec, orphan=True)
                    for e in b.entries)
    s.blk = Block(key=None, gen=gen, legacy=False,
                  held=(None,) * s.cfg.n_mods, pload=None,
                  entries=orphans, arena=b.arena)
    s.rw = None


def r_ra_begin(s):
    s.spec = s.blk


def g_spec(s):
    return s.spec is not None


def restore_dirty(s):
    for c in range(s.cfg.n_slots):
        if s.dirty[c]:
            s.s_card = tset(s.s_card, c, s.s_disk[c])
    s.dirty = (False,) * s.cfg.n_slots
    s.queue = ()


def r_ra_rollback(s):
    s.blk = s.spec
    s.spec = None
    restore_dirty(s)


def r_ra_adopt(s):
    s.spec = None
    wrote = False
    for c in range(s.cfg.n_slots):
        if s.dirty[c]:
            s.s_disk = tset(s.s_disk, c, s.s_card[c])
            wrote = True
    s.dirty = (False,) * s.cfg.n_slots
    q, s.queue = s.queue, ()
    for op in q:
        commit_op(s, op)
    if wrote:
        s.rw = None


def r_ra_adopt_fail(s):
    s.spec = None
    s.dirty = (False,) * s.cfg.n_slots
    if s.queue:
        log(s, "adoption-flush-failed")
    s.queue = ()


def r_ra_partial(s):
    s.spec = None
    restore_dirty(s)


def r_rw_mark(s):
    s.rw = s.blk


def g_rewind(s):
    return s.spec is None and s.rw is not None


def r_rewind(s):
    s.blk = s.rw


def r_edit(s, slot, idx, content):
    s.s_disk = tset(s.s_disk, slot, tset(s.s_disk[slot], idx, content))


def g_edit(s, slot, idx, content):
    return s.spec is None and s.s_disk[slot][idx] != content


def g_sc_corrupt(s, slot):
    return s.spec is None and s.sc_disk[slot] not in (None, "CORRUPT")


def r_sc_corrupt(s, slot):
    s.sc_disk = tset(s.sc_disk, slot, "CORRUPT")


def g_sc_fault(s):
    return not s.sc_fault


def r_sc_fault(s):
    s.sc_fault = True


def r_handoff(s, loaded, entered):
    r_ss_take(s)
    r_restart(s, loaded, entered)
    r_ss_load(s)


def g_modset(s, slot, tag):
    return s.spec is None and s.modset[slot] != tag


def r_modset(s, slot, tag):
    s.modset = tset(s.modset, slot, tag)


TICK = "tick"
HOST = "host"

RULES = {
    "T":           (TICK, g_tick, r_tick),
    "LOAD":        (TICK, g_load, r_load),
    "LOAD_STALE":  (TICK, g_load, r_load_stale),
    "RESET":       (TICK, g_tick, r_reset),
    "WRITE":       (TICK, g_write, r_write),
    "WRITE_FAIL":  (TICK, g_write, r_write_fail),
    "ISSUE":       (TICK, g_issue, r_issue),
    "COMPLETE":    (TICK, g_complete, r_complete),
    "FILE_CREATE": (TICK, g_mounted, r_file_create),
    "FORMAT":      (TICK, g_mounted, r_format),
    "ISSUE_NOREC": (TICK, g_mounted, r_issue_norec),
    "ABORT":       (TICK, g_abort, r_abort),
    "PLAY":        (TICK, g_play, r_play),
    "TOGGLE":      (HOST, g_toggle, r_toggle),
    "ENTER":       (HOST, g_enter, r_enter),
    "SCHEMA":      (HOST, g_schema, r_schema),
    "RESTART":     (HOST, lambda s, *a: s.spec is None, r_restart),
    "MOUNT":       (HOST, g_mount, r_mount),
    "SS_TAKE":     (HOST, g_quiet, r_ss_take),
    "SS_LOAD":     (HOST, g_ss_load, r_ss_load),
    "SS_LOAD_OLD": (HOST, g_ss_load, r_ss_load_old),
    "RA_BEGIN":    (HOST, g_quiet, r_ra_begin),
    "RA_ROLLBACK": (HOST, g_spec, r_ra_rollback),
    "RA_ADOPT":    (HOST, g_spec, r_ra_adopt),
    "RA_ADOPT_FAIL": (HOST, g_spec, r_ra_adopt_fail),
    "RA_PARTIAL":  (HOST, g_spec, r_ra_partial),
    "RW_MARK":     (HOST, g_quiet, r_rw_mark),
    "REWIND":      (HOST, g_rewind, r_rewind),
    "EDIT":        (HOST, g_edit, r_edit),
    "SC_CORRUPT":  (HOST, g_sc_corrupt, r_sc_corrupt),
    "SC_FAULT":    (HOST, g_sc_fault, r_sc_fault),
    "HANDOFF":     (HOST, lambda s, *a: s.spec is None, r_handoff),
    "MOUNT_ADOPT": (HOST, g_mount, r_mount_adopt),
    "MODSET":      (HOST, g_modset, r_modset),
}


def enabled(s, ev):
    kind, args = ev[0], ev[1:]
    return RULES[kind][1](s, *args)


def apply(s, ev):
    """Apply one event to a copy of s; returns (new state, effects list)."""
    kind, args = ev[0], ev[1:]
    n = clone(s)
    RULES[kind][2](n, *args)
    return n, n.effects


def is_tick(ev):
    return RULES[ev[0]][0] == TICK


# ── canonical form ─────────────────────────────────────────────────────────

class _Canon:
    def __init__(self):
        self.roots = {}
        self.lins = []

    def collect(self, lin):
        if lin is not None and lin[0] not in self.roots:
            self.roots[lin[0]] = len(self.roots)
        if lin is not None:
            self.lins.append(lin)


def _walk_lins(s, visit):
    def blk(b):
        if b is None:
            return
        if isinstance(b.key, tuple):
            visit(b.key[2])
        for h in b.held:
            if h is not None and h[0] is not None:
                visit(h[0])
        for e in b.entries:
            if e is not None and e.lin is not None:
                visit(e.lin)

    def table(t):
        if t in (None, "CORRUPT"):
            return
        for snaps in t:
            for x in snaps:
                visit(x.lin)
    blk(s.blk)
    for t in s.sc_mem:
        table(t)
    for t in s.sc_disk:
        table(t)
    for op in s.queue:
        if op[0] in ("snap", "seed"):
            visit(op[3].lin)
    blk(s.spec)
    blk(s.ss)
    blk(s.rw)


def canon(s):
    """Hashable canonical form: lineage roots renamed by first occurrence, each
    root's counter shifted to start at 0, generations kept as equality with
    their block's current generation."""
    order = {}
    low = {}

    def visit(lin):
        if lin[0] not in order:
            order[lin[0]] = len(order)
        low[lin[0]] = min(low.get(lin[0], lin[1]), lin[1])
    _walk_lins(s, visit)

    def L(lin):
        return None if lin is None else (order[lin[0]], lin[1] - low[lin[0]])

    def snap(x):
        return (x.content, L(x.lin), x.tag, x.recs)

    def table(t):
        if t in (None, "CORRUPT"):
            return t
        return tuple(tuple(snap(x) for x in snaps) for snaps in t)

    def blk(b):
        if b is None:
            return None
        key = b.key if not isinstance(b.key, tuple) else (b.key[0], b.key[1], L(b.key[2]), b.key[3])
        held = tuple(None if h is None else (L(h[0]), h[1] == b.gen) for h in b.held)
        pl = None if b.pload is None else (tuple(sorted(b.pload[0])),) + b.pload[1:]
        ents = tuple(None if e is None else
                     (e.slot, e.idx, e.gen == b.gen, e.content, L(e.lin), e.recs, e.tag,
                      e.captured, e.fresh, e.en, e.norec, e.orphan)
                     for e in b.entries)
        return (key, b.legacy, held, pl, ents, b.arena)

    q = tuple((op[0], op[1]) if op[0] == "format" else
              (op[0], op[1], op[2]) + ((snap(op[3]),) if len(op) > 3 else ())
              for op in s.queue)
    return (blk(s.blk), s.mods, s.armed, s.mounted, s.s_card, s.s_disk, s.dirty,
            tuple(table(t) for t in s.sc_mem), tuple(table(t) for t in s.sc_disk),
            s.sc_fault, s.modset, q, blk(s.spec), blk(s.ss), blk(s.rw))
