#!/usr/bin/env python3
"""Fence: thread-role ownership (core/include/openpete_thread_roles.h).

Check 1 (analysis). Every selected (file, flags) entry of the linux-release
and windows-mingw compile databases is compiled by clang-19 -fsyntax-only
with -Werror=thread-safety -Werror=ignored-attributes and
SDL_THREAD_SAFETY_ANALYSIS undefined, over a line-preserving rewritten copy
of the TU served through -ivfsoverlay (diagnostics name the source file and
line). The rewrite:
  - a non-call reference to a role-annotated function passed to an
    OP_SYNC_FN parameter (or to qsort/bsearch) becomes `(helper(), f)`,
    where the helper carries f's role attributes, so the analysis checks
    the reference as a call at that site;
  - a call through an OP_SLOT(<role>) slot becomes `(helper(), slot)(...)`
    with a helper that requires <role>;
  - every OP_SEALED global is declared const outside the functions
    OP_SEAL_TABLE lists for its seal, so a write site (assignment, compound
    assignment, increment, a non-const pointer or reference to it or to an
    array member) fails to compile. A dropped-const warning
    (discards-qualifiers, cast-qual) on a line naming a sealed global is a
    write site too.

Check 3 (binding table), read by libclang 14 from macro-instantiation
cursors and the AST:
  - OP_ROLE_BIND / OP_ROLE_ASSERT / OP_ROLE_ASSERT_SHARED / OP_ROLE_ENTER
    only in a function OP_ROLE_BINDING_TABLE lists, naming a role of its
    row; a role macro inside another macro's body; a direct call of the
    functions those macros expand to;
  - every non-call reference to a role-relevant function classified as
    synchronous, slot, entry (the row's registration form) or test, any
    other refused; a row function carrying role attributes;
  - a read of an OP_SLOT slot that is not a call, a test or a store into a
    slot holding the source slot's roles;
  - a call of a boot function outside openpete_run and run_split_threaded;
    a call of co_switch outside openpete_fiber_switch;
  - declarations of one function whose role attributes differ; a local
    prototype of an annotated function; a declaration of a sealed global
    without OP_SEALED;
  - OP_NO_THREAD_SAFETY_ANALYSIS outside OP_TSA_ACCESSOR_TABLE;
    OP_TSA_SUPPRESS_BEGIN without a non-empty string reason;
  - hook only: an added line that adds or changes a table row or a
    global's form (OP_GUARDED_BY*, OP_PT_GUARDED_BY*, OP_SEALED) without a
    `thread-roles: <reason>` marker.

Check 4 (engagement, LAW 8). TUs analysed per database, annotated
functions and globals, rows, accessors and escapes evaluated and references
classified are printed; zero TUs to analyse while in-scope sources changed,
a missing or unparsable database, or a selected TU that does not parse
under clang is VACUOUS and refuses. A binding-table, accessor or escape row
neither database defines is reported (check --all).

The escape table (OP_ESCAPE_TABLE) lists the functions that reach owned
state through a pointer the analysis does not follow, each with the
protocol that orders the accesses; it is reported, not checked.

scripts/thread_roles_annotate.py runs check 1 to a fixed point and writes
the OP_REQUIRES / OP_REQUIRES_SHARED attributes its diagnostics ask for.

Modes:
  hook            git pre-commit: the staged in-scope sources; a staged
                  header (or this fence, or the roles header) selects every
                  TU. Exit 1 refuses the commit.
  check           every build: the TUs changed since the last passing check
                  (a stamp beside the database); a changed header selects
                  every TU. --all selects every TU. --ctest: 64 without a
                  database.
  check --armed   exit 1 unless the tracked pre-commit hook calls this fence.
  sites           every check site in the tree (kind, file, line).
  join <report>   an OPENPETE_THREAD_ROLE_REPORT file joined to the site
                  list; a site no run reached shows as zero.

Environment:
  OPENPETE_THREAD_ROLES_DB        linux-release database path.
  OPENPETE_THREAD_ROLES_MINGW_DB  windows-mingw database path; missing, the
                                  windows-mingw preset is configured.
  OPENPETE_THREAD_ROLES_JOBS      parallel TUs (default half the CPUs, at most 16).
  OPENPETE_NO_COMMITS=1           `check --armed` passes unarmed.
  OPENPETE_THREAD_ROLES_NO_REWRITE=1  calibration control: synchronous
                                  references and slot calls are not
                                  rewritten into helper calls, so check 1
                                  does not see them as calls.
"""
import concurrent.futures
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time

import ratchet_lib as lib
from ratchet_lib import Refusal, say, git, is_header, entry_flags

SCOPE_ROOTS = ("core/", "games/", "tests/")
FENCE = lib.Fence("thread-roles", "thread_roles.py", "", SCOPE_ROOTS,
                  "OPENPETE_THREAD_ROLES_DB")
ROLES_H = "core/include/openpete_thread_roles.h"
RUNTIME_C = "core/runtime/thread_roles.c"
MINGW_DB = "build/windows-mingw/compile_commands.json"
MINGW_CONFIGURE = ["cmake", "--preset", "windows-mingw"]
STAMP = "thread_roles.stamp"
CLANG = {"c": "clang-19", "c++": "clang++-19"}
MINGW_TARGET = "--target=x86_64-w64-mingw32"
LIBCLANG_RESOURCE = os.path.join(os.path.dirname(lib.LIBCLANG), "clang", "14.0.0")
CLANG_CHECK = ["-fsyntax-only", "-Wno-unknown-warning-option",
               "-Wno-everything", "-Werror=thread-safety",
               "-Werror=ignored-attributes",
               "-Wincompatible-pointer-types-discards-qualifiers",
               "-Wcast-qual", "-USDL_THREAD_SAFETY_ANALYSIS", "-ferror-limit=0",
               "-fno-caret-diagnostics", "-fno-diagnostics-color"]

ATTR_MACROS = ("OP_REQUIRES", "OP_REQUIRES_SHARED", "OP_REQUIRES_CAP",
               "OP_REQUIRES_SHARED_CAP", "OP_ACQUIRE_CAP", "OP_RELEASE_CAP",
               "OP_ACQUIRE_SHARED_CAP", "OP_RELEASE_SHARED_CAP")
NTSA = "OP_NO_THREAD_SAFETY_ANALYSIS"
STMT_MACROS = ("OP_ROLE_BIND", "OP_ROLE_ASSERT", "OP_ROLE_ASSERT_SHARED",
               "OP_ROLE_ENTER")
FORM_MACROS = ("OP_GUARDED_BY", "OP_PT_GUARDED_BY", "OP_GUARDED_BY_CAP",
               "OP_PT_GUARDED_BY_CAP", "OP_SEALED")
BOOT_FNS = ("op_role_boot_acquire", "op_role_boot_release",
            "op_role_boot_handoff", "op_role_boot_reacquire")
BOOT_CALLERS = ("c:@F@openpete_run", "c:run.c@F@run_split_threaded")
SWITCH_USR = "c:@F@openpete_fiber_switch"
# The functions the statement macros expand to; called only from inside
# one of those macros.
MACRO_TARGETS = re.compile(r"^op_role__(bind|assert|assert_shared)_\w+$|"
                           r"^op_role_(bind|enter)_bits$")
SYNC_CALLEES = ("qsort", "bsearch")
MARKER = re.compile(r"thread-roles:\s*\S")
IDENT = re.compile(r"[A-Za-z_]\w*")


# --------------------------------------------------------------------------
# The tables and the text-level scan
# --------------------------------------------------------------------------
class Tables:
    """The roles header's tables and the tree's annotated declarations,
    read from text."""

    def __init__(self, root, files):
        hdr = read_text(os.path.join(root, ROLES_H))
        self.roles = re.findall(r"^\s*X\((\w+),\s*\d+\)", hdr, re.M)
        self.rows = {}
        for usr, form, roles in re.findall(
                r'ROW\("([^"]+)",\s*"([^"]+)",\s*"([^"]+)"\)', hdr):
            self.rows[usr] = (form, set(roles.split("|")))
        self.seal_writers = {}
        for seal, usr in re.findall(r'SEAL\((\w+),\s*"([^"]+)"\)', hdr):
            self.seal_writers.setdefault(seal, set()).add(usr)
        self.accessors = set(re.findall(r'ACCESSOR\("([^"]+)"\)', hdr))
        self.escapes = dict(re.findall(r'ESCAPE\("([^"]+)",\s*"([^"]+)"\)', hdr))
        # name -> seal, over every declaration that carries OP_SEALED
        self.sealed = {}
        self.sealed_files = set()
        self.candidates = set()     # names of possibly role-annotated functions
        self.slot_names = set()     # slot declarator names (fields, variables, parameters)
        self.slot_walk = set()      # the names a TU spells to store into or call a slot
        self.nested = []            # (file, line, macro) role macro in a #define
        self.guarded = 0            # declarations carrying an OP_GUARDED_BY form
        for rel in files:
            if rel == ROLES_H:
                continue
            try:
                text = read_text(os.path.join(root, rel))
            except OSError:
                continue
            if "OP_" not in text:
                continue
            self._scan(rel, text)
        self.candidates |= {usr_name(u) for u in self.rows}

    def _scan(self, rel, text):
        text = strip_comments(text)
        for m in re.finditer(r"\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*OP_SEALED\((\w+)\)", text):
            line = text[text.rfind("\n", 0, m.start()) + 1:m.start()]
            if line.lstrip().startswith("#"):
                continue
            static = re.match(r"\s*static\b", line) is not None
            self.sealed[(rel if static else None, m.group(1))] = m.group(2)
            self.sealed_files.add(rel)
        self.guarded += len(re.findall(r"\bOP_(?:PT_)?GUARDED_BY(?:_CAP)?\(", text))
        for m in re.finditer(r"\bOP_SLOT\(\w+\)", text):
            # The slot's declarator name; for a parameter slot also the
            # function whose parameter it is, the name its callers spell.
            stop = max(text.rfind(c, 0, m.start()) for c in ";}{")
            decl = text[stop + 1:m.start()]
            # `T name OP_SLOT` or a function-pointer declarator `T (*name)(...) OP_SLOT`
            ptr = re.findall(r"\(\s*\*\s*([A-Za-z_]\w*)\s*\)", decl)
            last = re.search(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*$", decl)
            name = last.group(1) if last else (ptr[-1] if ptr else "")
            if not name:
                continue
            self.slot_names.add(name)
            depth, opened = 0, None
            for k in range(len(decl) - 1, -1, -1):
                if decl[k] == ")":
                    depth += 1
                elif decl[k] == "(":
                    if depth == 0:
                        opened = k
                        break
                    depth -= 1
            if opened is not None:
                fn = re.search(r"([A-Za-z_]\w*)\s*$", decl[:opened])
                if fn:
                    self.slot_walk.add(fn.group(1))
            else:
                self.slot_walk.add(name)
        for m in re.finditer(r"\b(%s)\(" % "|".join(ATTR_MACROS), text):
            stop = max(text.rfind(c, 0, m.start()) for c in ";}{")
            stmt = text[stop + 1:m.start()]
            self.candidates |= set(re.findall(r"([A-Za-z_]\w*)\s*\(", stmt))
        for m in re.finditer(r"^[ \t]*#[ \t]*define\b(?:.*\\\n)*.*", text, re.M):
            for mm in re.finditer(r"\b(%s)\s*\(" % "|".join(STMT_MACROS), m.group(0)):
                line = text.count("\n", 0, m.start() + mm.start()) + 1
                self.nested.append((rel, line, mm.group(1)))


def read_text(path):
    with open(path, errors="replace") as fh:
        return fh.read()


COMMENT_OR_STRING = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|/\*.*?\*/|//[^\n]*', re.S)


def strip_comments(text):
    """text with every comment replaced by blanks (newlines kept, so line
    numbers hold); string and character literals are left as they are."""
    def blank(m):
        t = m.group(0)
        if t.startswith(("/*", "//")):
            return re.sub(r"[^\n]", " ", t)
        return t
    return COMMENT_OR_STRING.sub(blank, text)


def usr_name(usr):
    return usr.rsplit("@", 1)[-1]


# --------------------------------------------------------------------------
# Compile databases
# --------------------------------------------------------------------------
def load_entries(root, path, mingw):
    """-> {rel: [flags]} of the in-scope sources, one flag list per distinct
    (file, flags) entry."""
    with open(path) as fh:
        try:
            db = json.load(fh)
        except ValueError:
            raise Refusal("compile database %s does not parse; re-configure" % path)
    out = {}
    for e in db:
        f = os.path.normpath(os.path.join(e["directory"], e["file"]))
        rel = os.path.relpath(f, root)
        if rel.startswith("..") or not FENCE.in_scope(rel) or is_header(rel):
            continue
        flags = entry_flags(e)
        flags = [t for t in flags if not t.startswith(("-fdiagnostics-color",
                                                       "-fmax-errors"))]
        if mingw:
            flags = [MINGW_TARGET] + flags
        lst = out.setdefault(rel, [])
        if (e["directory"], flags) not in lst:
            lst.append((e["directory"], flags))
    return out


def mingw_db(root):
    path = os.environ.get("OPENPETE_THREAD_ROLES_MINGW_DB") or os.path.join(root, MINGW_DB)
    if os.path.isfile(path):
        return path
    if os.environ.get("OPENPETE_THREAD_ROLES_MINGW_DB"):
        raise Refusal("OPENPETE_THREAD_ROLES_MINGW_DB=%s does not exist" % path)
    say("thread-roles: no windows-mingw database; configuring: %s" % " ".join(MINGW_CONFIGURE))
    p = subprocess.run(["nice", "-n", "10"] + MINGW_CONFIGURE, cwd=root,
                       capture_output=True, text=True)
    if p.returncode != 0 or not os.path.isfile(path):
        raise Refusal("configuring windows-mingw failed (rc %d): %s"
                      % (p.returncode, p.stderr.strip()[-800:]))
    # The headers a build rule generates, which tracked sources include.
    p = subprocess.run(["nice", "-n", "10", "cmake", "--build", os.path.dirname(path),
                        "--target", "openpete_generated_headers"], cwd=root,
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise Refusal("building the windows-mingw generated headers failed (rc %d): %s"
                      % (p.returncode, p.stdout.strip()[-800:]))
    return path


# --------------------------------------------------------------------------
# Per-TU analysis (worker process)
# --------------------------------------------------------------------------
_CI = None


def ci():
    global _CI
    if _CI is None:
        _CI = lib.libclang()
    return _CI


def lang_of(rel, flags):
    if rel.endswith((".cpp", ".cc", ".cxx")):
        return "c++"
    return "c"


_CXX_INCLUDES = {}


def cxx_includes(mingw):
    """-> the C++ standard library directories of the build's compiler (g++
    or the mingw g++), which libclang 14 does not find on its own."""
    if mingw not in _CXX_INCLUDES:
        cxx = "x86_64-w64-mingw32-g++-posix" if mingw else "g++"
        try:
            p = subprocess.run([cxx, "-x", "c++", "-E", "-v", "-"], input="",
                               capture_output=True, text=True)
            text = p.stderr
        except OSError:
            text = ""
        block = text.split("#include <...> search starts here:")[-1].split("End of search list.")[0]
        _CXX_INCLUDES[mingw] = [os.path.normpath(d.strip()) for d in block.splitlines()
                                if "/c++" in d]
    return _CXX_INCLUDES[mingw]


def macro_args(cur):
    toks = [t.spelling for t in cur.get_tokens()]
    if len(toks) >= 3 and toks[1] == "(":
        return toks[2:-1] if toks[-1] == ")" else toks[2:]
    return []


def annotations(cur):
    out = []
    for ch in cur.get_children():
        if ch.kind == ci().CursorKind.ANNOTATE_ATTR:
            out.append(ch.spelling)
    return out


def slot_role(cur):
    for a in annotations(cur):
        if a.startswith("op_slot:"):
            return a.split(":", 1)[1]
    return None


class Walk:
    """One TU's libclang pass: role macros, declarations, references."""

    def __init__(self, ctx, rel, directory, flags):
        self.ctx = ctx
        self.rel = rel
        self.main = os.path.normpath(os.path.join(ctx["root"], rel))
        self.refusals = []
        self.decl_attrs = {}      # usr -> [(file:line, attrs tuple)]
        self.rows_defined = set()
        self.kinds = {}           # reference kind -> count
        self.edits = []           # (line, col_start, col_end, replacement)
        self.helpers = {}         # (line, col) of the enclosing definition -> [text]
        self.helper_names = {}    # helper -> original name
        self.annotated = {}       # usr -> attrs tuple
        self.slot_stores = []     # (usr, name, slot roles) of every slot store
        self.n = 0
        K = ci().CursorKind
        args = list(flags) + ["-Wno-everything"]
        if MINGW_TARGET in flags:
            # libclang 14 finds its builtin headers (stdbool.h, stdarg.h) for
            # the host target only; name its resource directory for mingw.
            args += ["-resource-dir", LIBCLANG_RESOURCE]
        if lang_of(rel, flags) == "c++":
            for d in cxx_includes(MINGW_TARGET in flags):
                args += ["-isystem", d]
        self.tu = ci().Index.create().parse(
            self.main, args=args,
            options=ci().TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD)
        # A libclang error leaves holes in the AST the walk reads: VACUOUS.
        self.parse_errors = ["%s (libclang 14)" % d for d in self.tu.diagnostics
                             if d.severity >= 3][:3]
        self.macros = []          # (file, line, col, name, args, extent end)
        self.defs = []            # main-file definitions: (start, end, cursor)
        self.fn_by_usr = {}
        tops = list(self.tu.cursor.get_children())
        for c in tops:
            f = c.location.file
            if c.kind == K.MACRO_INSTANTIATION:
                if c.spelling in ATTR_MACROS + STMT_MACROS + (NTSA, "OP_SEALED",
                                                              "OP_TSA_SUPPRESS_BEGIN"):
                    self.macros.append((f.name if f else "", c.location.line,
                                        c.location.column, c.spelling, macro_args(c),
                                        (c.extent.end.line, c.extent.end.column)))
        self.macros_by_file = {}
        for m in self.macros:
            self.macros_by_file.setdefault(m[0], []).append(m)
        self.stack = list(tops)
        self._collect_decls(tops)

    # ---- declarations --------------------------------------------------
    def _attrs_of(self, c):
        """Role attribute macros written on this declaration (before the body)."""
        f = c.location.file
        if not f:
            return ()
        end = (c.extent.end.line, c.extent.end.column)
        for ch in c.get_children():
            if ch.kind == ci().CursorKind.COMPOUND_STMT:
                end = (ch.extent.start.line, ch.extent.start.column)
        start = (c.extent.start.line, c.extent.start.column)
        out = []
        for m in self.macros_by_file.get(f.name, ()):
            if m[3] in ATTR_MACROS + (NTSA,) and start <= (m[1], m[2]) <= end:
                out.append("%s(%s)" % (m[3], "".join(m[4])) if m[3] != NTSA else NTSA)
        return tuple(sorted(out))

    def _collect_decls(self, tops):
        K = ci().CursorKind
        fn = (K.FUNCTION_DECL, K.CXX_METHOD)
        stack = list(tops)
        while stack:
            c = stack.pop()
            if c.kind in (K.UNEXPOSED_DECL, K.LINKAGE_SPEC, K.NAMESPACE):
                stack.extend(c.get_children())
                continue
            f = c.location.file
            if not f:
                continue
            if c.kind in fn:
                usr = c.get_usr()
                attrs = self._attrs_of(c)
                rel = os.path.relpath(f.name, self.ctx["root"])
                # Every declaration is kept: one without attributes beside an
                # annotated one is the mismatch the report refuses.
                if attrs or FENCE.in_scope(rel):
                    self.decl_attrs.setdefault(usr, []).append(
                        ("%s:%d" % (rel, c.location.line), attrs))
                if attrs:
                    self.annotated[usr] = attrs
                if c.is_definition():
                    if f.name == self.main:
                        self.defs.append(((c.extent.start.line, c.extent.start.column),
                                          (c.extent.end.line, c.extent.end.column), c))
                    if usr in self.ctx["rows"] or usr in self.ctx["escapes"] \
                            or usr in self.ctx["accessors"]:
                        self.rows_defined.add(usr)
                    if NTSA in attrs and usr not in self.ctx["accessors"] \
                            and rel != RUNTIME_C:
                        self.refuse(c, "%s on %s, which OP_TSA_ACCESSOR_TABLE does "
                                    "not list" % (NTSA, c.spelling))
            elif c.kind == K.VAR_DECL:
                rel = os.path.relpath(f.name, self.ctx["root"])
                seal = self.ctx["sealed"].get((rel, c.spelling)) \
                    or (self.ctx["sealed"].get((None, c.spelling))
                        if c.linkage == ci().LinkageKind.EXTERNAL else None)
                if seal and FENCE.in_scope(rel) and not any(
                        m[3] == "OP_SEALED" and m[1] == c.location.line
                        and m[2] > c.location.column
                        for m in self.macros_by_file.get(f.name, ())):
                    self.refuse(c, "declaration of the sealed global %s without "
                                "OP_SEALED(%s)" % (c.spelling, seal))

    def refuse(self, c, msg):
        f = c.location.file
        where = "%s:%d" % (os.path.relpath(f.name, self.ctx["root"]) if f else self.rel,
                           c.location.line)
        self.refusals.append("%s: %s" % (where, msg))

    def enclosing_def(self, line, col):
        best = None
        for s, e, c in self.defs:
            if s <= (line, col) <= e:
                best = (s, e, c)
        return best

    # ---- statement macros ---------------------------------------------
    def check_role_macros(self):
        for f, line, col, name, args, _ in self.macros:
            if f != self.main:
                continue
            if name == "OP_TSA_SUPPRESS_BEGIN":
                if not args or not re.match(r'^"[^"]+"$', "".join(args)):
                    self.refusals.append("%s:%d: OP_TSA_SUPPRESS_BEGIN needs a non-empty "
                                         "string reason" % (self.rel, line))
                continue
            if name not in STMT_MACROS:
                continue
            role = args[0] if args else "?"
            enc = self.enclosing_def(line, col)
            if not enc:
                self.refusals.append("%s:%d: %s(%s) outside a function"
                                     % (self.rel, line, name, role))
                continue
            usr = enc[2].get_usr()
            row = self.ctx["rows"].get(usr)
            if not row:
                self.refusals.append(
                    "%s:%d: %s(%s) in %s, which OP_ROLE_BINDING_TABLE does not list "
                    "(%s)" % (self.rel, line, name, role, enc[2].spelling, usr))
            elif role not in row[1]:
                self.refusals.append(
                    "%s:%d: %s(%s) in %s, whose row allows %s"
                    % (self.rel, line, name, role, enc[2].spelling, "|".join(sorted(row[1]))))

    # ---- the body walk --------------------------------------------------
    def walk(self):
        K = ci().CursorKind
        stmt_macro_at = {(m[1], m[2]) for m in self.macros
                         if m[0] == self.main and m[3] in STMT_MACROS}
        for top in self.stack:
            f = top.location.file
            if not f or f.name != self.main:
                continue
            path = []
            self._visit(top, path, stmt_macro_at)

    def _visit(self, root_cur, path, stmt_macro_at):
        K = ci().CursorKind
        todo = [(root_cur, 0)]
        chain = []
        while todo:
            c, depth = todo.pop()
            del chain[depth:]
            chain.append(c)
            kind = c.kind
            if kind in (K.DECL_REF_EXPR, K.MEMBER_REF_EXPR):
                self._ref(c, chain, stmt_macro_at)
            elif kind == K.CALL_EXPR:
                self._call(c, chain, stmt_macro_at)
            elif kind == K.FUNCTION_DECL and depth > 0:
                usr = c.get_usr()
                if usr in self.annotated:
                    self.refuse(c, "local prototype of the annotated function %s" % c.spelling)
            kids = list(c.get_children())
            for k in reversed(kids):
                todo.append((k, depth + 1))

    def _use(self, chain):
        """-> (index of the first ancestor that is not a cast, paren or
        address-of, that ancestor)."""
        K = ci().CursorKind
        skip = (K.UNEXPOSED_EXPR, K.PAREN_EXPR, K.CSTYLE_CAST_EXPR,
                K.CXX_FUNCTIONAL_CAST_EXPR, K.CXX_STATIC_CAST_EXPR,
                K.CXX_REINTERPRET_CAST_EXPR)
        i = len(chain) - 2
        while i >= 0:
            a = chain[i]
            if a.kind in skip:
                i -= 1
                continue
            if a.kind == K.UNARY_OPERATOR:
                toks = list(a.get_tokens())
                if toks and toks[0].spelling == "&":
                    i -= 1
                    continue
            return i, a
        return -1, None

    def _child_index(self, parent, child):
        for i, ch in enumerate(parent.get_children()):
            if ch == child:
                return i
        return -1

    def _binop(self, c):
        kids = list(c.get_children())
        if len(kids) != 2:
            return None
        end = kids[0].extent.end.offset
        for t in c.get_tokens():
            if t.extent.start.offset >= end:
                return t.spelling
        return None

    def _call(self, c, chain, stmt_macro_at):
        ref = c.referenced
        if ref is None:
            self._slot_call(c)
            return
        name = ref.spelling
        enc = self.enclosing_def(c.location.line, c.location.column)
        enc_usr = enc[2].get_usr() if enc else ""
        if name in BOOT_FNS and enc_usr not in BOOT_CALLERS:
            self.refuse(c, "call of the boot function %s outside openpete_run and "
                        "run_split_threaded" % name)
        if name == "co_switch" and enc_usr != SWITCH_USR:
            self.refuse(c, "co_switch outside openpete_fiber_switch")
        if MACRO_TARGETS.match(name) and self.rel != RUNTIME_C \
                and (c.location.line, c.location.column) not in stmt_macro_at:
            self.refuse(c, "direct call of %s; write the OP_ROLE_* macro" % name)
        self._slot_call(c)

    def _slot_call(self, c):
        """A call through an OP_SLOT field, variable or parameter: rewrite the
        callee as a helper call requiring the slot's role."""
        K = ci().CursorKind
        kids = list(c.get_children())
        if not kids:
            return
        callee = kids[0]
        while callee.kind in (K.UNEXPOSED_EXPR, K.PAREN_EXPR) and list(callee.get_children()):
            callee = list(callee.get_children())[0]
        if callee.kind not in (K.DECL_REF_EXPR, K.MEMBER_REF_EXPR):
            return
        tgt = callee.referenced
        if tgt is None or tgt.kind == K.FUNCTION_DECL:
            return
        role = slot_role(tgt)
        if not role:
            return
        self.kinds["slot-call"] = self.kinds.get("slot-call", 0) + 1
        self._rewrite(callee, "op__slot_%s" % tgt.spelling, ("OP_REQUIRES(%s)" % role,),
                      tgt.spelling)

    def _rewrite(self, expr, base, attrs, original):
        s, e = expr.extent.start, expr.extent.end
        if not s.file or s.file.name != self.main or s.line != e.line \
                or s.offset >= e.offset:
            self.refusals.append("%s:%d: the reference to %s lies in a macro expansion or "
                                 "spans lines; it cannot be checked as a call"
                                 % (self.rel, s.line, original))
            return
        enc = self.enclosing_def(s.line, s.column)
        if not enc:
            self.refusals.append("%s:%d: the reference to %s is outside a function"
                                 % (self.rel, s.line, original))
            return
        self.n += 1
        helper = "%s_L%d_%d" % (base, s.line, self.n)
        self.helper_names[helper] = original
        text = read_text(self.main).split("\n")[s.line - 1]
        span = text[s.column - 1:e.column - 1]
        self.edits.append((s.line, s.column - 1, e.column - 1,
                           "(%s(), %s)" % (helper, span)))
        at = (enc[0][0], enc[0][1])
        self.helpers.setdefault(at, []).append(
            "static inline void %s(void) %s {} " % (helper, " ".join(attrs)))

    def _ref(self, c, chain, stmt_macro_at):
        K = ci().CursorKind
        tgt = c.referenced
        if tgt is not None and tgt.kind in (K.VAR_DECL, K.PARM_DECL, K.FIELD_DECL) \
                and tgt.spelling in self.ctx["slot_names"]:
            role = slot_role(tgt)
            if role:
                self._slot_read(c, chain, role)
            return
        if tgt is None or tgt.kind not in (K.FUNCTION_DECL, K.CXX_METHOD):
            return
        i, use = self._use(chain)
        if use is not None and use.kind == K.CALL_EXPR \
                and self._child_index(use, chain[i + 1]) == 0:
            return   # a call; _call handles it
        name = tgt.spelling
        usr = tgt.get_usr()
        attrs = self.annotated.get(usr) or ()
        internal = name in BOOT_FNS or MACRO_TARGETS.match(name) \
            or name in ("op_role_leave", "op_role_check_at")
        row = self.ctx["rows"].get(usr)
        if not (attrs or row or internal or name == "co_switch"):
            # A slot store is recorded so the annotator never gives the
            # function a role the slot does not hold.
            _, _, slot = self._classify(c, chain, i, use)
            if slot is not None:
                self.slot_stores.append((usr, name, slot))
            return
        if internal or name == "co_switch":
            self.refuse(c, "non-call reference to %s" % name)
            return
        kind, form, slot = self._classify(c, chain, i, use)
        if kind == "test":
            self._count("test")
            return
        if row:
            if attrs:
                self.refuse(c, "%s is a binding-table entry and carries %s; an entry "
                            "asserts or binds its role instead" % (name, ", ".join(attrs)))
            if row[0] in form:
                self._count("entry")
            else:
                self.refuse(c, "%s is registered through %s; its row lists %s"
                            % (name, ", ".join(sorted(form)) or "an unlisted form", row[0]))
            return
        if kind == "sync":
            self._count("sync")
            self._rewrite(c, "op__ref_%s" % name, attrs, name)
            return
        if slot is not None:
            self.slot_stores.append((usr, name, slot))
            need = self._roles_of(attrs)
            if need is None or not need <= {slot}:
                self.refuse(c, "%s (%s) stored into an OP_SLOT(%s) slot"
                            % (name, ", ".join(attrs), slot))
            else:
                self._count("slot")
            return
        self.refuse(c, "non-call reference to the role-annotated %s (%s) is neither "
                    "synchronous, a slot store, a binding-table form nor a test"
                    % (name, ", ".join(attrs)))

    def _slot_read(self, c, chain, role):
        """A read of an OP_SLOT slot: a call (rewritten by _slot_call), a
        test, or a store into a slot whose roles hold the source's."""
        K = ci().CursorKind
        i, use = self._use(chain)
        if use is not None and use.kind == K.CALL_EXPR \
                and self._child_index(use, chain[i + 1]) == 0:
            return
        if use is not None and use.kind == K.BINARY_OPERATOR and self._binop(use) == "=" \
                and self._child_index(use, chain[i + 1]) == 0:
            return   # the slot is the store's target
        kind, _, slot = self._classify(c, chain, i, use)
        if kind == "test":
            self._count("slot-test")
        elif slot is not None and set(role.split("|")) <= set(slot.split("|")):
            self._count("slot-copy")
        else:
            self.refuse(c, "read of the OP_SLOT(%s) slot %s is neither a call, a test nor "
                        "a store into a slot holding its roles" % (role, c.spelling))

    def _count(self, k):
        self.kinds[k] = self.kinds.get(k, 0) + 1

    def _roles_of(self, attrs):
        out = set()
        for a in attrs:
            m = re.match(r"(OP_REQUIRES|OP_REQUIRES_SHARED)\((\w+)\)$", a)
            if not m:
                return None
            out.add(m.group(2))
        return out

    def _classify(self, c, chain, i, use):
        """-> (kind, set of registration forms, slot role or None)."""
        K = ci().CursorKind
        forms = set()
        if use is None:
            return "other", forms, None
        if use.kind == K.UNARY_OPERATOR:
            toks = list(use.get_tokens())
            if toks and toks[0].spelling == "!":
                return "test", forms, None
        if use.kind == K.BINARY_OPERATOR:
            op = self._binop(use)
            if op in ("==", "!=", "&&", "||"):
                return "test", forms, None
            if op == "=":
                lhs = list(use.get_children())[0]
                while lhs.kind in (K.UNEXPOSED_EXPR, K.PAREN_EXPR) and list(lhs.get_children()):
                    lhs = list(lhs.get_children())[0]
                tgt = lhs.referenced if lhs.kind in (K.MEMBER_REF_EXPR, K.DECL_REF_EXPR) else None
                if tgt is not None:
                    if tgt.kind == K.FIELD_DECL:
                        forms.add("field:%s.%s" % (record_name(tgt.semantic_parent),
                                                   tgt.spelling))
                    return "store", forms, slot_role(tgt)
        if use.kind in (K.IF_STMT, K.WHILE_STMT, K.CONDITIONAL_OPERATOR) \
                and self._child_index(use, chain[i + 1]) == 0:
            return "test", forms, None
        if use.kind == K.CALL_EXPR:
            idx = self._child_index(use, chain[i + 1]) - 1
            callee = use.referenced
            cname = callee.spelling if callee is not None else ""
            forms.add("arg:%s" % cname)
            if cname == "SDL_SetPointerProperty" and idx == 2:
                forms.add("prop:%s" % "".join(call_args_text(use)[1:2]))
            if cname in SYNC_CALLEES:
                return "sync", forms, None
            if callee is not None:
                params = list(callee.get_arguments())
                if 0 <= idx < len(params):
                    ann = annotations(params[idx])
                    if "op_sync_fn" in ann:
                        return "sync", forms, None
                    role = slot_role(params[idx])
                    if role:
                        return "store", forms, role
            return "arg", forms, None
        if use.kind == K.VAR_DECL:
            return "init", forms, slot_role(use)
        if use.kind == K.INIT_LIST_EXPR:
            forms.add("init:%s" % type_name(use.type))
            field = self._init_field(use, chain[i + 1])
            if field is not None:
                return "init", forms, slot_role(field)
            return "init", forms, None
        return "other", forms, None

    def _init_field(self, lst, child):
        K = ci().CursorKind
        kids = list(lst.get_children())
        for ch in child.get_children():
            if ch.kind == K.MEMBER_REF:
                return ch.referenced
        idx = self._child_index(lst, child)
        try:
            fields = list(lst.type.get_canonical().get_fields())
        except Exception:
            return None
        if 0 <= idx < len(fields) and len(kids) <= len(fields):
            return fields[idx]
        return None


def call_args_text(call):
    """-> the source spelling of each argument of a call, split at the
    top-level commas of its token stream (macro arguments stay unexpanded)."""
    toks = [t.spelling for t in call.get_tokens()]
    if "(" not in toks:
        return []
    out, cur, depth = [], [], 0
    for t in toks[toks.index("(") + 1:]:
        if t in "([{":
            depth += 1
        elif t in ")]}":
            if depth == 0:
                break
            depth -= 1
        if t == "," and depth == 0:
            out.append("".join(cur))
            cur = []
        else:
            cur.append(t)
    out.append("".join(cur))
    return out


def record_name(c):
    n = c.spelling or c.type.spelling
    return re.sub(r"^(struct|union|const)\s+", "", n)


def type_name(t):
    n = t.spelling
    n = re.sub(r"\b(const|volatile|struct|union)\s+", "", n)
    return n.strip()


def sealed_names(sealed, rel):
    """The sealed names visible in rel: every external one, and rel's own
    statics."""
    return sorted({n for (f, n) in sealed if f is None or f == rel})


def rewrite_sealed(text, sealed, writers_spans, lang):
    """Line-preserving: `const ` before each OP_SEALED declarator, and the
    sealed names inside the listed writers' bodies cast back to writable."""
    lines = text.split("\n")
    pat = re.compile(r"\b([A-Za-z_]\w*)(\s*(?:\[[^\]]*\]\s*)*)OP_SEALED\((\w+)\)")
    for n, line in enumerate(lines):
        if "OP_SEALED(" in line and not line.lstrip().startswith("#"):
            lines[n] = pat.sub(lambda m: "const %s%sOP_SEALED(%s)"
                               % (m.group(1), m.group(2), m.group(3)), line)
    if writers_spans:
        names = list(sealed)
        if names:
            npat = re.compile(r"(?<![.>\w])(?<!op_seal_check\()(?<!op_seal\()(%s)\b"
                              % "|".join(map(re.escape, names)))
            for (l0, l1) in writers_spans:
                for n in range(l0 - 1, min(l1, len(lines))):
                    if "OP_SEALED(" in lines[n]:
                        continue
                    if lang == "c":
                        rep = r"(*(__typeof_unqual__(\1)*)(__UINTPTR_TYPE__)&(\1))"
                    else:
                        rep = r"(const_cast<__remove_const(decltype(\1))&>(\1))"
                    lines[n] = npat.sub(rep, lines[n])
    return "\n".join(lines)


def writer_spans(text, writer_names):
    """-> [(first line, last line)] of the bodies of the named functions'
    definitions in text (a brace-matching scan that skips comments, strings
    and character literals)."""
    spans = []
    if not writer_names:
        return spans
    for m in re.finditer(r"\b(%s)\s*\(" % "|".join(map(re.escape, writer_names)), text):
        # the definition: the parameter list closes, then '{' before any ';'
        i, depth = m.end(), 1
        while i < len(text) and depth:
            depth += {"(": 1, ")": -1}.get(text[i], 0)
            i += 1
        j = i
        while j < len(text) and text[j] not in "{;":
            j += 1
        if j >= len(text) or text[j] != "{":
            continue
        line_start = text.count("\n", 0, m.start())
        prefix = text[text.rfind("\n", 0, m.start()) + 1:m.start()]
        if not re.fullmatch(r"\s*(?:[A-Za-z_]\w*[\s*]+)*", prefix) \
                or re.search(r"\b(return|if|while|else|case)\b", prefix):
            continue
        k, depth = j + 1, 1
        while k < len(text) and depth:
            ch = text[k]
            if text.startswith("/*", k):
                k = text.find("*/", k + 2) + 2 or len(text)
                continue
            if text.startswith("//", k):
                k = text.find("\n", k) if text.find("\n", k) >= 0 else len(text)
                continue
            if ch in "\"'":
                q, k = ch, k + 1
                while k < len(text) and text[k] != q:
                    k += 2 if text[k] == "\\" else 1
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
            k += 1
        spans.append((line_start + 1, text.count("\n", 0, k) + 1))
    return spans


def apply_edits(text, edits, helpers):
    lines = text.split("\n")
    by_line = {}
    for line, a, b, rep in edits:
        by_line.setdefault(line, []).append((a, b, rep))
    for (line, col), hs in helpers.items():
        by_line.setdefault(line, []).append((col - 1, col - 1, "".join(hs)))
    for line, es in by_line.items():
        s = lines[line - 1]
        for a, b, rep in sorted(es, key=lambda x: (x[0], x[1]), reverse=True):
            s = s[:a] + rep + s[b:]
        lines[line - 1] = s
    return "\n".join(lines)


def overlay_yaml(pairs):
    roots = []
    for orig, repl in pairs:
        roots.append({"name": os.path.dirname(orig), "type": "directory",
                      "contents": [{"name": os.path.basename(orig), "type": "file",
                                    "external-contents": repl}]})
    return json.dumps({"version": 0, "use-external-names": False, "roots": roots})


# Errors the const rewrite produces at a write site (C and C++ spellings).
SEAL_ERR = re.compile(r"const-qualified|read-only|discards qualifiers|drops '?const|"
                      r"cast from .* drops|cannot (initialize|bind)|no matching")


def analyse(task):
    """Worker: one (rel, db name, directory, flags) entry. -> result dict."""
    ctx, rel, dbname, directory, flags = task
    t0 = time.time()
    res = {"rel": rel, "db": dbname, "refusals": [], "parse": [], "decl_attrs": {},
           "rows_defined": [], "kinds": {}, "walked": False, "rewritten": False,
           "tsa": 0, "seal": 0, "diags": [], "slot_stores": []}
    root = ctx["root"]
    main = os.path.join(root, rel)
    text = read_text(main)
    lang = lang_of(rel, flags)
    tmp = tempfile.mkdtemp(prefix="thread-roles-", dir=ctx["tmp"])
    pairs = []
    rewritten_main = None
    need_walk = (ctx["walk_re"].search(text) is not None
                 or rel in ctx["sealed_files"] or rel in ctx["writer_files"]
                 or (ctx["redecl_re"] is not None and ctx["redecl_re"].search(text)))
    if need_walk:
        try:
            w = Walk(ctx, rel, directory, flags)
        except Exception as e:   # libclang could not parse at all
            res["parse"].append("%s: libclang parse failed: %s" % (rel, e))
            return res
        res["walked"] = True
        res["parse"] += w.parse_errors
        w.check_role_macros()
        w.walk()
        res["refusals"] += w.refusals
        res["decl_attrs"] = w.decl_attrs
        res["rows_defined"] = sorted(w.rows_defined)
        res["kinds"] = w.kinds
        res["slot_stores"] = w.slot_stores
        if (w.edits or w.helpers) and not ctx["no_rewrite"]:
            rewritten_main = apply_edits(text, w.edits, w.helpers)
        helper_names = w.helper_names
    else:
        helper_names = {}
    # Sealed declarations: the main file and every header that declares one.
    for f in ctx["sealed_files"] | ({rel} if rewritten_main is not None else set()):
        path = os.path.join(root, f)
        body = rewritten_main if (f == rel and rewritten_main is not None) \
            else read_text(path)
        spans = writer_spans(body, ctx["writer_names"]) if f == rel else []
        new = rewrite_sealed(body, sealed_names(ctx["sealed"], rel), spans, lang)
        if f == rel:
            rewritten_main = new
        if new != read_text(path):
            out = os.path.join(tmp, "%d_%s" % (len(pairs), os.path.basename(f)))
            with open(out, "w") as fh:
                fh.write(new)
            pairs.append((os.path.normpath(path), out))
    args = [CLANG[lang]] + list(flags) + CLANG_CHECK
    if pairs:
        y = os.path.join(tmp, "overlay.yaml")
        with open(y, "w") as fh:
            fh.write(overlay_yaml(pairs))
        args += ["-ivfsoverlay", y]
        res["rewritten"] = True
    args.append(main)
    p = subprocess.run(args, cwd=directory, capture_output=True, text=True)
    lines_cache = {}

    def src_line(path, n):
        if path not in lines_cache:
            try:
                lines_cache[path] = read_text(path).split("\n")
            except OSError:
                lines_cache[path] = []
        ls = lines_cache[path]
        return ls[n - 1] if 0 < n <= len(ls) else ""

    visible = sealed_names(ctx["sealed"], rel)
    sealed_re = re.compile(r"(?<![.>\w])(%s)\b" % "|".join(map(re.escape, visible))) \
        if visible else None
    for line in p.stderr.splitlines():
        m = re.match(r"^(.+?):(\d+):(\d+): (error|warning): (.*)$", line)
        if not m:
            if "error:" in line:
                res["parse"].append(line)
            continue
        path, ln, sev, msg = m.group(1), int(m.group(2)), m.group(4), m.group(5)
        path = os.path.normpath(os.path.join(directory, path))
        relp = os.path.relpath(path, root)
        for h, orig in helper_names.items():
            msg = msg.replace("'%s'" % h, "'%s' (referenced here)" % orig)
        if "-Wthread-safety" in msg:
            res["refusals"].append("%s:%d: %s" % (relp, ln, msg))
            res["tsa"] += 1
            res["diags"].append((relp, ln, int(m.group(3)), msg))
        elif sealed_re and sealed_re.search(src_line(path, ln)) and (
                SEAL_ERR.search(msg) or "discards-qualifiers" in msg or "cast-qual" in msg
                or "redeclaration of" in msg):
            res["refusals"].append("%s:%d: write site of a sealed global outside its "
                                   "listed writers: %s" % (relp, ln, msg))
            res["seal"] += 1
        elif sev == "error":
            res["parse"].append("%s:%d: %s" % (relp, ln, msg))
    if p.returncode != 0 and not res["refusals"] and not res["parse"]:
        res["parse"].append("%s: clang-19 exit %d: %s" % (rel, p.returncode,
                                                         p.stderr.strip()[-400:]))
    res["sec"] = time.time() - t0
    return res


# --------------------------------------------------------------------------
# Selection, the run, the report
# --------------------------------------------------------------------------
def tracked_scope(root):
    return sorted(lib.present_scope_files(root, FENCE))


def scope_hashes(root, files):
    out = {}
    for f in files + [ROLES_H, "scripts/fences/thread_roles.py",
                      "scripts/fences/ratchet_lib.py"]:
        try:
            st = os.stat(os.path.join(root, f))
            out[f] = "%d:%d" % (st.st_mtime_ns, st.st_size)
        except OSError:
            out[f] = "-"
    return out


def build_ctx(root, tables, tmp):
    walk_names = set(tables.candidates) | set(BOOT_FNS) | {"co_switch"} \
        | set(tables.slot_walk) | set(STMT_MACROS) | {"OP_TSA_SUPPRESS_BEGIN", NTSA}
    walk_names = {n for n in walk_names if n and n not in ("if", "while", "for", "sizeof",
                                                          "return", "switch")}
    writer_usrs = set().union(*tables.seal_writers.values()) if tables.seal_writers else set()
    writer_files = set()
    for u in writer_usrs:
        m = re.match(r"c:([^@]+)@F@", u)
        if m:
            writer_files.add(m.group(1))
    return {"root": root, "rows": tables.rows, "accessors": tables.accessors,
            "escapes": tables.escapes,
            "sealed": tables.sealed, "sealed_files": set(tables.sealed_files),
            "writer_names": sorted({usr_name(u) for u in writer_usrs}),
            "writer_files_base": writer_files, "writer_files": set(),
            "no_rewrite": os.environ.get("OPENPETE_THREAD_ROLES_NO_REWRITE") == "1",
            "walk_names": sorted(walk_names), "slot_names": set(tables.slot_names),
            "tmp": tmp}


def compile_patterns(ctx):
    ctx["walk_re"] = re.compile(r"\b(%s)\b" % "|".join(map(re.escape, ctx["walk_names"])))
    ext = sorted({n for (f, n) in ctx["sealed"] if f is None})
    ctx["redecl_re"] = (re.compile(r"\bextern\b[^;]*\b(%s)\b" % "|".join(
        map(re.escape, ext))) if ext else None)


def run_analysis(root, select, jobs, label):
    """select: {db name: {rel: [(directory, flags)]}} -> results."""
    files = tracked_scope(root)
    tables = Tables(root, files)
    tmp = tempfile.mkdtemp(prefix="thread-roles-")
    ctx = build_ctx(root, tables, tmp)
    ctx["writer_files"] = {f for f in files
                           if os.path.basename(f) in ctx["writer_files_base"]}
    compile_patterns(ctx)
    tasks = []
    for dbname, entries in select.items():
        for rel, lst in sorted(entries.items()):
            for directory, flags in lst:
                tasks.append((ctx, rel, dbname, directory, flags))
    results = []
    t0 = time.time()
    if tasks:
        with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as ex:
            for r in ex.map(analyse, tasks, chunksize=1):
                results.append(r)
    wall = time.time() - t0
    subprocess.run(["rm", "-rf", "--", tmp])
    return tables, ctx, results, wall


def report(root, tables, ctx, results, wall, selected_files, mode):
    refusals, parse = [], []
    per_db = {}
    attrs = {}
    rows_defined = set()
    kinds = {}
    walked = rewritten = tsa = seal = 0
    for r in results:
        per_db.setdefault(r["db"], []).append(r)
        refusals += r["refusals"]
        parse += r["parse"]
        for usr, lst in r["decl_attrs"].items():
            attrs.setdefault(usr, set()).update((w, a) for w, a in lst)
        rows_defined.update(r["rows_defined"])
        for k, v in r["kinds"].items():
            kinds[k] = kinds.get(k, 0) + v
        walked += r["walked"]
        rewritten += r["rewritten"]
        tsa += r["tsa"]
        seal += r["seal"]
    # declarations whose role attributes differ (per USR, over every TU)
    for usr, decls in sorted(attrs.items()):
        sets = {tuple(x for x in a if x != NTSA) for _, a in decls}
        if len(sets) > 1 and any(sets):
            refusals.append("%s: declarations carry different role attributes: %s"
                            % (usr, "; ".join("%s %s" % (w, " ".join(a) or "(none)")
                                              for w, a in sorted(decls))))
    for rel, line, mac in tables.nested:
        if rel in selected_files:
            refusals.append("%s:%d: %s inside a macro body; role macros are written at "
                            "their site" % (rel, line, mac))
    refusals = sorted(set(refusals))
    say("thread-roles: %s, %.1f s wall" % (mode, wall))
    for db in sorted(per_db):
        rs = per_db[db]
        cpu = sum(r.get("sec", 0) for r in rs)
        say("  %-6s TUs analysed %d (%d distinct files), CPU %.1f s, %.2f s per entry"
            % (db, len(rs), len({r["rel"] for r in rs}), cpu, cpu / len(rs)))
    n_annot_fns = sum(1 for usr, d in attrs.items() if any(a for _, a in d))
    say("  check 1: annotated functions seen %d, annotated globals %d (sealed %d, "
        "guarded %d), TUs rewritten %d, thread-safety diagnostics %d, sealed write "
        "sites %d" % (n_annot_fns, len(tables.sealed) + tables.guarded, len(tables.sealed),
                      tables.guarded, rewritten, tsa, seal))
    say("  check 3: TUs walked %d, rows %d (defined in an analysed TU %d), accessors %d "
        "(defined %d), escapes %d (defined %d), references classified %s"
        % (walked, len(tables.rows), len(rows_defined & set(tables.rows)),
           len(tables.accessors), len(rows_defined & tables.accessors),
           len(tables.escapes), len(rows_defined & set(tables.escapes)),
           ", ".join("%s %d" % kv for kv in sorted(kinds.items())) or "none"))
    listed = set(tables.rows) | tables.accessors | set(tables.escapes)
    unseen = sorted(listed - rows_defined)
    if unseen and mode.startswith("check --all"):
        say("  table rows no analysed TU defines: %s" % ", ".join(unseen))
    slow = sorted(results, key=lambda r: -r.get("sec", 0))[:5]
    if results:
        say("  slowest TUs: %s" % ", ".join("%s[%s] %.2f s" % (r["rel"], r["db"], r.get("sec", 0))
                                             for r in slow))
    if parse:
        say("thread-roles: VACUOUS: %d selected TU(s) do not parse under clang-19:" % len(parse))
        for p in parse[:20]:
            say("    " + p)
    if refusals:
        say("thread-roles: %d refusal(s):" % len(refusals))
        for r in refusals[:60]:
            say("    " + r)
        if len(refusals) > 60:
            say("    ... %d more" % (len(refusals) - 60))
    return 1 if (refusals or parse) else 0, unseen


def databases(root, need_mingw=True):
    path = os.environ.get(FENCE.db_env) or os.path.join(root, lib.DB_DEFAULT)
    if not os.path.isfile(path):
        return None, None
    linux = load_entries(root, path, False)
    mingw = load_entries(root, mingw_db(root), True) if need_mingw else {}
    return linux, mingw


def jobs():
    default = max(1, min(16, (os.cpu_count() or 2) // 2))
    try:
        return max(1, int(os.environ.get("OPENPETE_THREAD_ROLES_JOBS", default)))
    except ValueError:
        return default


def select_all(linux, mingw, files=None):
    sel = {"linux": {}, "mingw": {}}
    for name, entries in (("linux", linux), ("mingw", mingw)):
        for rel, lst in entries.items():
            if files is None or rel in files:
                sel[name][rel] = lst
    return sel


def marker_refusals(root, base, staged):
    """Hook: added lines that add or change a table row or a global's form
    need a `thread-roles:` reason on the line."""
    out = []
    hunks = lib.diff_hunks(root, base, staged)
    for path, hs in lib.added_ranges(hunks).items():
        try:
            lines = read_text(os.path.join(root, path)).split("\n")
        except OSError:
            continue
        if not FENCE.in_scope(path):
            continue
        code_lines = strip_comments("\n".join(lines)).split("\n")
        for a, b in hs:
            for n in range(a, b + 1):
                s = lines[n - 1] if n - 1 < len(lines) else ""
                code = code_lines[n - 1] if n - 1 < len(code_lines) else ""
                if code.lstrip().startswith("#"):
                    continue
                table = path == ROLES_H and re.search(r"\b(ROW|SEAL|ACCESSOR|ESCAPE)\(\"?\w", code)
                form = path != ROLES_H and re.search(r"\b(%s)\(" % "|".join(FORM_MACROS), code)
                if (table or form) and not MARKER.search(s):
                    out.append("%s:%d: a row or a global's form changes without a "
                               "`thread-roles: <reason>` marker on the line" % (path, n))
    return out


def mode_hook(root):
    lib.refuse_temporary_index()
    changes, base = lib.staged_changes(root)
    staged = [b or a for s, a, b in changes if s != "D"]
    scope = [f for f in staged if FENCE.in_scope(f) or f in (
        "scripts/fences/thread_roles.py", ROLES_H)]
    if not scope:
        say("thread-roles: no staged in-scope path")
        return 0
    dirty = set(git(root, "diff", "--name-only").stdout.split("\n"))
    unstaged = sorted(f for f in scope if f in dirty)
    if unstaged:
        raise Refusal("staged with unstaged hunks: %s. The fence reads the working "
                      "file and the commit carries the index blob. `git add` them, "
                      "or stash the rest." % ", ".join(unstaged))
    linux, mingw = databases(root)
    if linux is None:
        raise Refusal("VACUOUS: no linux-release compile database. Run: %s" % lib.CONFIGURE)
    whole = any(is_header(f) for f in scope) or "scripts/fences/thread_roles.py" in scope
    sources = {f for f in scope if not is_header(f) and FENCE.in_scope(f)}
    missing = sorted(f for f in sources if f not in linux and f not in mingw
                     and f not in lib.UNSCORED)
    if missing:
        raise Refusal("VACUOUS: staged sources with no compile-database entry: %s. "
                      "Re-run: %s" % (", ".join(missing), lib.CONFIGURE))
    sel = select_all(linux, mingw, None if whole else sources)
    mode = "hook (%s)" % ("whole tree: a staged header" if whole
                          else "%d staged source(s)" % len(sources))
    if not any(sel.values()) and sources:
        raise Refusal("VACUOUS: zero TUs analysed for %d staged source(s)" % len(sources))
    tables, ctx, results, wall = run_analysis(root, sel, jobs(), mode)
    rc, _ = report(root, tables, ctx, results, wall, set(scope), mode)
    marks = marker_refusals(root, base, scope)
    for m in marks:
        say("    " + m)
    if marks:
        rc = 1
    if rc:
        say("thread-roles: REFUSED")
    return rc


def mode_check(root, argv):
    ctest = "--ctest" in argv
    whole = "--all" in argv
    path = os.environ.get(FENCE.db_env) or os.path.join(root, lib.DB_DEFAULT)
    if not os.path.isfile(path):
        if ctest:
            say("thread-roles: SKIP: no compile database at %s" % path)
            return 64
        raise Refusal("VACUOUS: no compile database at %s. Run: %s" % (path, lib.CONFIGURE))
    linux, mingw = databases(root)
    files = tracked_scope(root)
    stamp_path = os.path.join(os.path.dirname(os.path.abspath(path)), STAMP)
    now = scope_hashes(root, files)
    db_sig = "%s %s" % (os.stat(path).st_mtime_ns, len(linux))
    try:
        with open(stamp_path) as fh:
            old = json.load(fh)
    except (OSError, ValueError):
        old = None
    if whole or old is None or old.get("db") != db_sig:
        sel_files = None
        why = "every TU (%s)" % ("--all" if whole else "no stamp" if old is None
                                 else "the database changed")
    else:
        changed = {f for f, h in now.items() if old["files"].get(f) != h}
        if any(is_header(f) or not f.startswith(SCOPE_ROOTS) for f in changed):
            sel_files, why = None, "every TU (a header or the fence changed)"
        else:
            sel_files = {f for f in changed if f in linux or f in mingw}
            why = "%d changed source(s)" % len(sel_files)
            if changed and not sel_files:
                say("thread-roles: check: the changed paths have no database entry "
                    "(%s)" % ", ".join(sorted(changed)[:5]))
    sel = select_all(linux, mingw, sel_files)
    mode = "check --all" if whole else "check: %s" % why
    if sel_files is not None and not sel_files:
        say("thread-roles: check: no analysed source changed since the last pass")
        with open(stamp_path, "w") as fh:
            json.dump({"db": db_sig, "files": now}, fh)
        return 0
    if not any(sel.values()):
        raise Refusal("VACUOUS: zero TUs selected (%s)" % why)
    tables, ctx, results, wall = run_analysis(root, sel, jobs(), mode)
    rc, _ = report(root, tables, ctx, results, wall,
                   set(files) if sel_files is None else sel_files, mode)
    if rc == 0:
        with open(stamp_path, "w") as fh:
            json.dump({"db": db_sig, "files": now}, fh)
        say("thread-roles: PASS")
    else:
        say("thread-roles: REFUSED")
    return rc


def mode_armed(root):
    if os.environ.get("OPENPETE_NO_COMMITS") == "1":
        say("thread-roles: OPENPETE_NO_COMMITS=1; hook arming not required.")
        return 0
    why = lib.armed_reason(root, ("thread_roles.py",))
    if why:
        say("thread-roles: DISARMED: %s.\n  Arm this clone once:  %s" % (why, lib.ARM))
        return 1
    say("thread-roles: ARMED (%s)" % lib.HOOK_REL)
    return 0


SITE_RE = re.compile(r"\b(OP_ROLE_BIND|OP_ROLE_ASSERT_SHARED|OP_ROLE_ASSERT|OP_ROLE_ENTER|"
                     r"OP_ROLE_CHECK|op_seal_check)\s*\(\s*(\w+)")
KIND = {"OP_ROLE_BIND": "bind", "OP_ROLE_ASSERT": "assert", "OP_ROLE_ASSERT_SHARED": "assert",
        "OP_ROLE_ENTER": "enter", "OP_ROLE_CHECK": "check", "op_seal_check": "seal"}


def site_list(root):
    out = []
    for rel in tracked_scope(root):
        if rel == ROLES_H:
            continue
        text = read_text(os.path.join(root, rel))
        for m in SITE_RE.finditer(text):
            line_start = text.rfind("\n", 0, m.start()) + 1
            if text[line_start:m.start()].lstrip().startswith("#"):
                continue
            out.append((KIND[m.group(1)], rel, text.count("\n", 0, m.start()) + 1, m.group(2)))
    return out


def mode_sites(root):
    for kind, rel, line, arg in site_list(root):
        print("%s\t%s\t%d\t%s" % (kind, rel, line, arg))
    return 0


def mode_join(root, argv):
    if len(argv) < 2:
        return None
    reached = {}
    for line in read_text(argv[1]).splitlines()[1:]:
        cols = line.split("\t")
        if len(cols) < 7:
            continue
        reached[(cols[1], int(cols[2]))] = cols
    print("kind\tfile\tline\tfunction\tevals\tfails")
    seen = set()
    for kind, rel, line, arg in site_list(root):
        hit = None
        for (f, ln), cols in reached.items():
            if ln == line and os.path.normpath(f).endswith(rel):
                hit = cols
                seen.add((f, ln))
        if hit:
            print("%s\t%s\t%d\t%s\t%s\t%s" % (kind, rel, line, hit[3], hit[5], hit[6]))
        else:
            print("%s\t%s\t%d\t-\t0\t0" % (kind, rel, line))
    for key, cols in sorted(reached.items()):
        if key not in seen:
            print("%s\t%s\t%s\t%s\t%s\t%s\t(not in the tree's site list)"
                  % (cols[0], cols[1], cols[2], cols[3], cols[5], cols[6]))
    return 0


def dispatch(root, mode, argv):
    if mode in ("hook", "check") and "--armed" not in argv \
            and not os.path.isfile(os.path.join(root, ROLES_H)):
        if os.path.realpath(root) == os.path.realpath(lib.OWN_ROOT):
            raise Refusal("%s is missing" % ROLES_H)
        say("thread-roles: %s holds no %s; nothing to check" % (root, ROLES_H))
        return 0
    if mode == "hook":
        return mode_hook(root)
    if mode == "check" and "--armed" in argv:
        return mode_armed(root)
    if mode == "check":
        return mode_check(root, argv)
    if mode == "sites":
        return mode_sites(root)
    if mode == "join":
        return mode_join(root, argv)
    return None


if __name__ == "__main__":
    sys.exit(lib.run(FENCE, dispatch, __doc__))
