#!/usr/bin/env python3
"""Annotator: role attributes to a fixed point (core/include/openpete_thread_roles.h).

Runs scripts/fences/thread_roles.py's check 1 (clang-19 -Wthread-safety over
the line-preserving rewrite, where a synchronous reference and a slot call
are helper calls) and reads its diagnostics. A diagnostic naming a role
(`requires holding role 'op_role_<r>'`) adds, to every declaration of the
function it lies in (matched by USR), OP_REQUIRES(<r>) when the diagnostic
says "exclusively" and OP_REQUIRES_SHARED(<r>) otherwise; an exclusive
diagnostic upgrades an OP_REQUIRES_SHARED(<r>). The analysis then re-runs
over the TUs that name a newly annotated function, until no diagnostic adds
an attribute, and a last pass runs every TU. The process only adds or
upgrades attributes over a finite set of (function, role) pairs, so it ends.

It never annotates a binding-table entry, an assert, bind, enter or boot
function, or a function stored into an OP_SLOT whose roles lack the role
(that would widen the slot). There it stops and prints the chain from the
diagnostic to the access that started it; the fence reports the same site
as a refusal. A diagnostic naming a capability that is not a role (a
module's lock object) is printed as unresolved: it is fixed by taking the
lock, not by propagation. A check 3 refusal of the last pass is printed too.

Every declaration is found by libclang 14 in the TUs that define or name
the function; the attribute goes directly after the parameter list's
closing parenthesis, the place the fence reads it from. A declaration whose
parameter list cannot be placed in in-scope text (a macro expansion, a
generated header) is a stop.

Usage:
  thread_roles_annotate.py [--db linux|mingw|both] [--dry-run] [--max-iter N]

Environment: as thread_roles.py (OPENPETE_THREAD_ROLES_DB,
OPENPETE_THREAD_ROLES_MINGW_DB, OPENPETE_THREAD_ROLES_JOBS).
Exit 0: fixed point with no stop, no unresolved diagnostic and no refusal.
Exit 1: a stop, an unresolved diagnostic or a refusal remains (printed).
Exit 2: the analysis did not run (no database, a TU does not parse).
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "fences"))

import ratchet_lib as lib          # noqa: E402
import thread_roles as tr          # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, ".."))

DIAG = re.compile(r"^(calling function|reading variable|writing variable|passing variable|"
                  r"reading the value pointed to by|writing the value pointed to by|"
                  r"returning variable|returning the value pointed to by) "
                  r"'([^']+)'( \(referenced here\))?(?: by reference)? requires holding "
                  r"(\w+) '([^']+)'( exclusively)?")
ATTR_RE = re.compile(rb"\bOP_REQUIRES(_SHARED)?\((\w+)\)")


def say(msg=""):
    print(msg, flush=True)


# --------------------------------------------------------------------------
# libclang: enclosing definitions and declarations by USR
# --------------------------------------------------------------------------
def parse(rel, directory, flags):
    ci = tr.ci()
    args = list(flags) + ["-Wno-everything"]
    if tr.MINGW_TARGET in flags:
        args += ["-resource-dir", tr.LIBCLANG_RESOURCE]
    cwd = os.getcwd()
    try:
        os.chdir(directory)
        return ci.Index.create().parse(os.path.join(ROOT, rel), args=args)
    finally:
        os.chdir(cwd)


def function_cursors(tu):
    """Every function declaration and definition the TU holds, in any file."""
    K = tr.ci().CursorKind
    out = []
    stack = list(tu.cursor.get_children())
    while stack:
        c = stack.pop()
        if c.kind in (K.UNEXPOSED_DECL, K.LINKAGE_SPEC, K.NAMESPACE, K.CLASS_DECL,
                      K.STRUCT_DECL):
            stack.extend(c.get_children())
            continue
        if c.kind in (K.FUNCTION_DECL, K.CXX_METHOD) and c.location.file:
            out.append(c)
    return out


class TUCache:
    """One libclang parse per (file, database) per iteration."""

    def __init__(self, dbs):
        self.dbs = dbs             # db name -> {rel: [(directory, flags)]}
        self.cache = {}

    def get(self, rel, db):
        if (rel, db) not in self.cache:
            d, f = self.dbs[db][rel][0]
            self.cache[(rel, db)] = function_cursors(parse(rel, d, f))
        return self.cache[(rel, db)]

    def clear(self):
        self.cache = {}


def enclosing(cursors, path, line):
    """-> the innermost definition in `path` whose extent holds `line`."""
    best = None
    for c in cursors:
        if not c.is_definition() or os.path.normpath(c.location.file.name) != path:
            continue
        s, e = c.extent.start.line, c.extent.end.line
        if s <= line <= e and (best is None or e - s < best.extent.end.line - best.extent.start.line):
            best = c
    return best


# --------------------------------------------------------------------------
# Text placement
# --------------------------------------------------------------------------
def skip_ws_comments(b, i):
    while i < len(b):
        if b[i:i + 2] == b"/*":
            j = b.find(b"*/", i + 2)
            i = len(b) if j < 0 else j + 2
        elif b[i:i + 2] == b"//":
            j = b.find(b"\n", i)
            i = len(b) if j < 0 else j
        elif b[i:i + 1] in b" \t\r\n\\":
            i += 1
        else:
            return i
    return i


def close_paren(b, i):
    """b[i] is '('; -> the offset just past its matching ')'."""
    depth = 0
    while i < len(b):
        ch = b[i:i + 1]
        if b[i:i + 2] == b"/*":
            j = b.find(b"*/", i + 2)
            i = len(b) if j < 0 else j + 2
            continue
        if b[i:i + 2] == b"//":
            j = b.find(b"\n", i)
            i = len(b) if j < 0 else j
            continue
        if ch in (b'"', b"'"):
            q, i = ch, i + 1
            while i < len(b) and b[i:i + 1] != q:
                i += 2 if b[i:i + 1] == b"\\" else 1
        elif ch == b"(":
            depth += 1
        elif ch == b")":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return -1


def line_offset(b, line, col):
    off = 0
    for _ in range(line - 1):
        off = b.index(b"\n", off) + 1
    return off + col - 1


def place(b, line, col, name, role, excl):
    """-> (new bytes, changed) with OP_REQUIRES[_SHARED](role) after the
    parameter list of the declaration whose name is at (line, col);
    raises ValueError when the text there is not that declaration."""
    at = line_offset(b, line, col)
    nm = name.encode()
    if b[at:at + len(nm)] != nm:
        raise ValueError("the name is not spelled at %d:%d (a macro expansion?)" % (line, col))
    i = skip_ws_comments(b, at + len(nm))
    if b[i:i + 1] != b"(":
        raise ValueError("no parameter list after the name at %d:%d" % (line, col))
    end = close_paren(b, i)
    if end < 0:
        raise ValueError("unbalanced parameter list at %d:%d" % (line, col))
    # the attribute region: up to the declaration's ';' or the body's '{'
    stop = len(b)
    for t in (b";", b"{"):
        k = b.find(t, end)
        if 0 <= k < stop:
            stop = k
    region = b[end:stop]
    want = ("OP_REQUIRES(%s)" if excl else "OP_REQUIRES_SHARED(%s)") % role
    for m in ATTR_RE.finditer(region):
        if m.group(2).decode() != role:
            continue
        if not m.group(1) or not excl:
            return b, False            # held already (exclusive covers shared)
        a, z = end + m.start(), end + m.end()
        return b[:a] + want.encode() + b[z:], True
    return b[:end] + b" " + want.encode() + b[end:], True


INCLUDE = b'#include "openpete_thread_roles.h"'


def ensure_include(b):
    """-> b with the roles header included after its last top-of-file
    #include, when it does not include it already."""
    if INCLUDE in b or b"openpete_thread_roles.h" in b:
        return b
    last = None
    first_brace = b.find(b"{")
    for m in re.finditer(rb"^[ \t]*#[ \t]*include\b[^\n]*\n", b, re.M):
        if 0 <= first_brace < m.start():
            break          # top-of-file includes only
        last = m
    at = last.end() if last else 0
    return b[:at] + INCLUDE + b"   /* OP_REQUIRES */\n" + b[at:]


# --------------------------------------------------------------------------
# The fixed point
# --------------------------------------------------------------------------
class Annotator:
    def __init__(self, which, dry):
        self.dry = dry
        linux, mingw = tr.databases(ROOT, need_mingw=which in ("mingw", "both"))
        if linux is None:
            raise lib.Refusal("no linux-release compile database; run %s" % lib.CONFIGURE)
        self.dbs = {}
        if which in ("linux", "both"):
            self.dbs["linux"] = linux
        if which in ("mingw", "both"):
            self.dbs["mingw"] = mingw
        self.all_dbs = {"linux": linux, "mingw": mingw or {}}
        self.entries = set(linux) | set(mingw or {})
        self.tus = TUCache(self.all_dbs)
        self.held = {}         # usr -> {role: "excl" | "shared"} written by this run
        self.reason = {}       # (usr, role) -> (where, what)
        self.names = {}        # usr -> spelling
        self.stops = []        # (where, entry name, why, chain start (usr, role))
        self.unresolved = []
        self.slot_roles = {}   # usr -> [slot roles]
        self.edits = 0
        self.iterations = 0

    def select(self, files):
        sel = {}
        for name, entries in self.dbs.items():
            sel[name] = {r: l for r, l in entries.items() if files is None or r in files}
        return sel

    def analyse(self, files):
        sel = self.select(files)
        n = sum(len(lst) for v in sel.values() for lst in v.values())
        tables, ctx, results, wall = tr.run_analysis(ROOT, sel, tr.jobs(), "annotate")
        say("annotate: pass %d, %d TU entries, %.1f s wall" % (self.iterations, n, wall))
        parse = [p for r in results for p in r["parse"]]
        if parse:
            for p in parse[:20]:
                say("    " + p)
            raise lib.Refusal("VACUOUS: %d TU(s) do not parse under clang-19" % len(parse))
        for r in results:
            for usr, name, slot in r.get("slot_stores", ()):
                self.slot_roles.setdefault(usr, set()).update(slot.split("|"))
        return tables, results

    def run(self, max_iter):
        files = None
        while True:
            self.iterations += 1
            if self.iterations > max_iter:
                raise lib.Refusal("no fixed point after %d passes" % max_iter)
            tables, results = self.analyse(files)
            self.tus.clear()
            want = self.collect(tables, results)
            if not want:
                if files is None:
                    return tables, results
                files = None           # confirm over every TU
                continue
            touched_names = self.apply(want)
            if self.dry:
                return tables, results
            files = self.files_naming(touched_names)

    def files_naming(self, names):
        pat = re.compile(r"\b(%s)\b" % "|".join(map(re.escape, sorted(names))))
        out = set()
        for rel in self.entries:
            try:
                if pat.search(tr.read_text(os.path.join(ROOT, rel))):
                    out.add(rel)
            except OSError:
                pass
        return out

    def collect(self, tables, results):
        """-> {usr: {role: excl bool}} the diagnostics ask for."""
        want = {}
        seen = set()
        for r in results:
            for relp, ln, col, msg in r["diags"]:
                key = (relp, ln, col, msg)
                if key in seen:
                    continue
                seen.add(key)
                m = DIAG.match(msg)
                where = "%s:%d" % (relp, ln)
                if not m or m.group(4) != "role" or not m.group(5).startswith("op_role_"):
                    self.unresolved.append("%s: %s" % (where, msg))
                    continue
                verb, obj, ref, _, cap, excl = m.groups()
                role = cap[len("op_role_"):]
                excl = bool(excl)
                cur = enclosing(self.tus.get(r["rel"], r["db"]), os.path.join(ROOT, relp), ln)
                if cur is None:
                    self.unresolved.append("%s: %s (in no function definition)" % (where, msg))
                    continue
                usr, name = cur.get_usr(), cur.spelling
                self.names[usr] = name
                what = "%s %s%s" % (verb, obj, " (a synchronous or slot reference)" if ref else "")
                why = self.refuse_reason(tables, usr, name, role)
                if why:
                    self.stops.append((where, name, why, what, (obj, role)))
                    continue
                if self.holds(usr, role, excl):
                    continue
                w = want.setdefault(usr, {})
                w[role] = w.get(role, False) or excl
                self.reason.setdefault((usr, role), (where, what, name))
        return want

    def refuse_reason(self, tables, usr, name, role):
        if usr in tables.rows:
            return "a binding-table entry (%s); it asserts or binds its role instead" \
                % tables.rows[usr][0]
        if name in tr.BOOT_FNS or tr.MACRO_TARGETS.match(name) \
                or name in ("op_role_leave", "op_role_check_at"):
            return "a role or boot function"
        slots = self.slot_roles.get(usr)
        if slots and role not in slots:
            return "stored into an OP_SLOT(%s) slot, which %s would widen" \
                % ("|".join(sorted(slots)), role)
        return None

    def holds(self, usr, role, excl):
        h = self.held.get(usr, {}).get(role)
        return h == "excl" or (h == "shared" and not excl)

    def declarations(self, usr, name):
        """-> {path: {(line, col)}} of every declaration of usr in the TUs that
        define or name it: their linux parse, and their mingw parse when the
        linux one finds none or the file has no linux entry."""
        out = {}
        naming = sorted(self.files_naming({name}))
        for db in ("linux", "mingw"):
            for rel in naming:
                if rel not in self.all_dbs[db]:
                    continue
                if db == "mingw" and out and rel in self.all_dbs["linux"]:
                    continue
                try:
                    cursors = self.tus.get(rel, db)
                except Exception as e:      # libclang could not parse
                    raise lib.Refusal("libclang parse of %s [%s] failed: %s" % (rel, db, e))
                for c in cursors:
                    if c.get_usr() == usr:
                        out.setdefault(os.path.normpath(c.location.file.name), set()).add(
                            (c.location.line, c.location.column))
        return out

    def apply(self, want):
        names = set()
        per_file = {}
        for usr, roles in sorted(want.items()):
            name = self.names[usr]
            decls = self.declarations(usr, name)
            bad = [p for p in decls if not p.startswith(ROOT + os.sep)
                   or not tr.FENCE.in_scope(os.path.relpath(p, ROOT))]
            if bad or not decls:
                for role in roles:
                    where, what, _ = self.reason[(usr, role)]
                    self.stops.append((where, name, "declared outside the in-scope tree (%s)"
                                       % ", ".join(sorted(bad)) if bad else "no declaration found",
                                       what, None))
                continue
            for role, excl in roles.items():
                h = self.held.setdefault(usr, {})
                h[role] = "excl" if excl or h.get(role) == "excl" else "shared"
                for p, locs in decls.items():
                    for line, col in locs:
                        per_file.setdefault(p, []).append((line, col, name, role, excl))
            names.add(name)
        for p, items in sorted(per_file.items()):
            with open(p, "rb") as fh:
                b = fh.read()
            orig = b
            # last position first, so earlier offsets hold
            for line, col, name, role, excl in sorted(items, reverse=True):
                try:
                    b, changed = place(b, line, col, name, role, excl)
                except ValueError as e:
                    self.stops.append(("%s:%d" % (os.path.relpath(p, ROOT), line), name,
                                       "cannot place the attribute: %s" % e, "", None))
                    continue
                if changed:
                    self.edits += 1
                    say("  + %s:%d %s %s(%s)" % (os.path.relpath(p, ROOT), line, name,
                                                  "OP_REQUIRES" if excl else "OP_REQUIRES_SHARED",
                                                  role))
            if b != orig:
                b = ensure_include(b)
            if b != orig and not self.dry:
                with open(p, "wb") as fh:
                    fh.write(b)
        return names

    def chain(self, start):
        """The reasons from a stopped edge back to the access that began it."""
        out = []
        obj, role = start
        guard = 0
        while obj and guard < 64:
            guard += 1
            hit = [(u, r) for (u, r) in self.reason if r == role and self.names.get(u) == obj]
            if not hit:
                break
            where, what, name = self.reason[hit[0]]
            out.append("%s at %s: %s" % (name, where, what))
            m = re.match(r"(calling function|reading variable|writing variable) (\S+)", what)
            if not m or not m.group(1).startswith("calling"):
                break
            obj = m.group(2)
        return out


def main(argv):
    which, dry, max_iter = "both", False, 40
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--db":
            which = argv[i + 1]
            i += 1
        elif a == "--dry-run":
            dry = True
        elif a == "--max-iter":
            max_iter = int(argv[i + 1])
            i += 1
        elif a in ("-h", "--help"):
            say(__doc__)
            return 0
        else:
            say("thread_roles_annotate: unknown argument %s" % a)
            return 2
        i += 1
    if which not in ("linux", "mingw", "both"):
        say("thread_roles_annotate: --db takes linux, mingw or both")
        return 2
    try:
        A = Annotator(which, dry)
        tables, results = A.run(max_iter)
    except lib.Refusal as e:
        say("annotate: %s" % e)
        return 2
    other = sorted({x for r in results for x in r["refusals"] if "-Wthread-safety" not in x})
    stop_sites = {w for w, *_ in A.stops}
    left = sorted({"%s:%d: %s" % (d[0], d[1], d[3]) for r in results for d in r["diags"]
                   if "%s:%d" % (d[0], d[1]) not in stop_sites})
    A.unresolved += left
    say("annotate: %d pass(es), %d attribute(s) written%s, %d function(s) annotated by this run"
        % (A.iterations, A.edits, " (dry run)" if dry else "", len(A.held)))
    stops = sorted(set((w, n, why, what, s) for w, n, why, what, s in A.stops))
    for where, name, why, what, start in stops:
        say("STOP %s: %s %s; %s is %s" % (where, name, what, name, why))
        if start:
            for step in A.chain(start):
                say("       because %s" % step)
    for u in sorted(set(A.unresolved)):
        say("UNRESOLVED %s" % u)
    for o in other:
        say("REFUSED %s" % o)
    return 1 if (stops or A.unresolved or other) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
