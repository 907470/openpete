#!/usr/bin/env python3
"""Fence: function-documentation ratchet.

Every in-scope function (core/ and games/ TUs of the linux-release compile
database) carries a doxygen comment at its canonical site, or a row in
baselines/docs/undocumented.tsv. A function meets the requirement when
exactly one doxygen-form comment (`/** */`, `/*! */`, `///`, `//!`)
attaches across its redeclarations in the TU, at the canonical site (the
declaration in a tracked project header when one is visible, else the
definition; a header `static inline` is its own site); the parsed comment
carries a `@brief` block command (either prefix, or its alias `@short`; a
leading paragraph is not a brief) and is not a group header (`@name`,
`@defgroup`, `@addtogroup`, `@ingroup`, `@{`); the first `@brief`
paragraph, which the Doxyfile (AUTOBRIEF off) takes whole as the brief, is
one sentence on one source line: no line break inside it and exactly one
sentence terminator (`.`, `!`, `?` before whitespace or the end, outside a
backtick code span and an abbreviation such as `e.g.`), at its end, so a
detailed description starts after a blank comment line (else `longbrief`);
no -Wdocumentation
diagnostic lies in it; and, for external linkage, a valid `@param` names
every named parameter of the canonical declaration and a non-void function
carries `@return`.

Decision over the acted-on keys (the functions of the staged TUs, plus
every function whose canonical site is a staged header):
  1. a key with no row that does not meet the requirement is refused;
  2. a key whose row exists and whose extent the commit touches is refused
     unless the function now meets the requirement (the row retires);
  3. a key whose row exists and whose extent is not touched: the row
     retires when the function meets the requirement, else it is kept
     with its form re-read.
A doxygen comment at a non-canonical redeclaration (`misplaced`), two
doxygen comments (`duplicate`), a function declared in two project
headers, and a -Wdocumentation diagnostic in a staged file or in the
canonical site of an acted-on key are refused.

Modes:
  hook          git pre-commit (githooks/pre-commit). Parses the working
                tree, decides over the index, writes and stages the baseline
                on PASS. Exit 1 aborts the commit.
  check         every build. The clauses over every key in a TU clean
                against HEAD, against HEAD's baseline. Exit 1 on a failure;
                64 = SKIP (no compile database) under --ctest.
  check --list  print every in-scope key with its bin and form, the
                NO-HEADER-INCLUDE and Windows-only lists; the population
                query of the fence.
  tighten       full-corpus rewrite at arc end: retirements and form
                changes; a key with no row that does not meet is refused.
  baseline      recapture the whole file (library-version change only).

Escape:
  OPENPETE_DOCS_ACCEPT="<reason>"   one-time acceptance of the functions
                                    the commit would refuse under clauses 1
                                    and 2 or as misplaced; the reason lands
                                    in each row's `accepted` column.
Environment:
  OPENPETE_DOCS_DB     compile database path (default
                       build/linux-release/compile_commands.json).
  OPENPETE_LIBCLANG    libclang shared library (default the LLVM 14 path).
"""
import json
import multiprocessing
import os
import re
import shutil
import subprocess
import sys
import time
from collections import defaultdict

import ratchet_lib as lib
from ratchet_lib import Refusal, say, git, is_header, key_file

LIB_MAJOR = 14
JOBS = 16
TSV = "baselines/docs/undocumented.tsv"
SCOPE_ROOTS = ("core/", "games/")
FENCE = lib.Fence("doc-ratchet", "doc_ratchet.py", TSV, SCOPE_ROOTS,
                  "OPENPETE_DOCS_DB")
ACCEPT_VAR = "OPENPETE_DOCS_ACCEPT"
DOC_FLAGS = ["-Wdocumentation", "-Wdocumentation-unknown-command"]
FORMS = ("none", "plain", "misplaced", "malformed", "longbrief", "untagged")
REQUIREMENT_LINE = ("@brief of one sentence on one line on every function; "
                    "@param per named parameter and @return on non-void for "
                    "external linkage")
FIXTURE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "tests", "doc_fixture")

# Definitions under #ifdef _WIN32 or a provides guard: invisible to the
# linux-release parse, governed by review. `check --list` prints them.
WINDOWS_ONLY = (
    "core/diag/crash.c:on_seh",
    "core/diag/crash.c:seh_name",
    "core/diag/crash.c:openpete_crash_main_tid",
    "core/host/host_spawn.c:win_spawn",
    "core/runtime/openpete_time.c:sleep_until_win",
    "core/runtime/openpete_time.c:thread_timer",
    "games/common/psyq/psyq_game_hooks_default.c:psyq_game_rand_seed_addr",
    "games/spyro-1/main.cpp:reconnect_std",
)

GROUP_CMDS = ("name", "defgroup", "addtogroup", "ingroup")
RETURN_CMDS = ("return", "returns", "result")
BRIEF_CMDS = ("brief", "short")
# Comment kinds (CXCommentKind).
K_TEXT, K_INLINE = 1, 2
K_PARAGRAPH, K_BLOCK, K_PARAM, K_VERBATIM_LINE, K_FULL = 5, 6, 7, 11, 12
FORM_RANK = {"meets": 0, "plain": 1, "none": 2, "untagged": 3,
             "longbrief": 4, "malformed": 4, "misplaced": 5, "duplicate": 6}
# A sentence terminator: `.`, `!` or `?` (a run counts once), optionally
# followed by closing punctuation, then whitespace or the end of the text.
TERMINATOR = re.compile(r"[.!?]+[)\]\"']*(?=\s|$)")
CODE_SPAN = re.compile(r"`[^`]*`")
# Abbreviations whose period is not a sentence terminator.
ABBREVIATIONS = re.compile(r"\b(?:e\.g|i\.e|etc|vs|cf|approx|resp)\.",
                           re.IGNORECASE)


# --------------------------------------------------------------------------
# The decision. Pure: every rule is a function of these inputs.
#
#   before_rows    {key: (form, accepted_reason)}      the baseline source
#   after_facts    {key: facts}                        the parsed working tree
#                  facts: form (meets | none | plain | misplaced | malformed
#                  | longbrief | untagged | duplicate), macro,
#                  two_headers [files],
#                  extents [(file, first, last)] (the definition, and the
#                  canonical header declaration), canonical (file, line)
#   acted_on       set of keys; None = every key
#   renames        {old_path: (new_path, identical_blob)}
#   changed_ranges {path: [(start, count)]}  after-side hunks of the index;
#                  count 0 = a pure deletion after line `start`
#   wired_files    paths with a compile-database entry (headers: all present)
#   present_files  paths on disk
#   accept_reason  str | None
#   staged_files   paths the commit stages
#   wdoc           [(file, line, message)] candidate-file diagnostics
#   unwired_all    refuse an UNWIRED row outside the acted-on set too
#
# Returns (verdict, new_rows, bins); verdict is "PASS" or "REFUSED"; bins
# maps a bin name to a list; REFUSED entries are (key or file:line, code,
# detail) with code in clause1 | clause2 | misplaced | duplicate |
# two-headers | wdoc | unwired.
# --------------------------------------------------------------------------
def touched(extents, changed_ranges):
    """An added line inside [first, last], or a pure deletion after line b
    with first <= b < last. Extents and hunks are after-side."""
    for f, lo, hi in extents:
        for start, count in changed_ranges.get(f, ()):
            if count:
                if start <= hi and start + count - 1 >= lo:
                    return True
            elif lo <= start < hi:
                return True
    return False


def decide(before_rows, after_facts, acted_on, renames, changed_ranges,
           wired_files, present_files, accept_reason, staged_files=(),
           wdoc=(), unwired_all=False):
    rows = {lib.carry_key(k, renames): v for k, v in before_rows.items()}
    identical = {new for new, same in renames.values() if same}
    bins = defaultdict(list)
    refused = []

    def acted(key):
        return acted_on is None or key in acted_on

    unwired = set()
    for key in sorted(rows):
        f = key_file(key)
        if (f in present_files and f not in wired_files
                and f not in identical and not is_header(f)
                and (unwired_all or acted(key))):
            unwired.add(key)
            refused.append((key, "unwired", f))

    new_rows = dict(rows)
    keys = sorted(k for k in set(rows) | set(after_facts)
                  if acted(k) and k not in unwired)
    for key in keys:
        fx = after_facts.get(key)
        row = rows.get(key)
        if fx is None:
            if row is not None and key_file(key) not in identical:
                del new_rows[key]
                bins["RETIRED"].append((key, row[0], "absent"))
            continue
        if fx["macro"]:
            bins["MACRO"].append(key)
            if row is not None:
                del new_rows[key]
                bins["RETIRED"].append((key, row[0], "MACRO"))
            continue
        form = fx["form"]
        code = None
        if fx.get("two_headers"):
            code = "two-headers"
        elif form == "duplicate":
            code = "duplicate"
        elif form == "meets":
            if row is not None:
                del new_rows[key]
                bins["RETIRED"].append((key, row[0], "meets"))
            continue
        elif form == "misplaced" and not (row and row[0] == form and row[1]):
            code = "misplaced"
        elif row is None:
            code = "clause1"
        elif touched(fx["extents"], changed_ranges):
            code = "clause2"
        if code is None:
            if row[0] != form:
                bins["FORM"].append((key, row[0], form))
                new_rows[key] = (form, row[1])
            continue
        if accept_reason and code in ("clause1", "clause2", "misplaced"):
            new_rows[key] = (form, accept_reason)
            bins["ACCEPTED"].append((key, row[0] if row else None, form))
            continue
        refused.append((key, code, form))

    canonical = {after_facts[k]["canonical"][0] for k in keys
                 if k in after_facts}
    staged = set(staged_files)
    for f, line, msg in wdoc:
        if f in staged or f in canonical:
            refused.append(("%s:%d" % (f, line), "wdoc", msg))
        else:
            bins["WDOC"].append("%s:%d  %s" % (f, line, msg))
    bins["REFUSED"] = refused
    return ("REFUSED" if refused else "PASS"), new_rows, bins


# --------------------------------------------------------------------------
# Baseline file
# --------------------------------------------------------------------------
def parse_tsv(text):
    """-> (header dict, {key: (form, accepted)})"""
    return lib.parse_header(text), {
        c[0] + ":" + c[1]: (c[2], c[3] if len(c) > 3 else "")
        for c in lib.parse_rows(text, 3)}


def render_tsv(rows, written_by, sha, version):
    return lib.render(
        "function-documentation ratchet baseline (scripts/fences/doc_ratchet.py)",
        [("library", version), ("requirement", REQUIREMENT_LINE)],
        ["file", "function", "form", "accepted"],
        [(k, [form, reason]) for k, (form, reason) in rows.items()],
        written_by, sha)


# --------------------------------------------------------------------------
# Scoring: one libclang parse per database entry, in a process pool
# --------------------------------------------------------------------------
CTX = {}


class Ctx:
    """What the scorer needs to classify a path. root: the repo (or fixture)
    root; scope(rel): an in-scope file whose definitions are keyed;
    candidate(rel): a file whose declaration may be a canonical site."""

    def __init__(self, root, scope, candidate):
        self.root = root
        self.scope = scope
        self.candidate = candidate


def repo_ctx(root):
    tracked = set(git(root, "ls-files").stdout.split("\n"))
    subs = []
    for line in git(root, "submodule", "status", check=False).stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            subs.append(parts[1].rstrip("/") + "/")

    def candidate(rel):
        return rel in tracked and not rel.startswith(tuple(subs))
    return Ctx(root, FENCE.in_scope, candidate)


def fixture_ctx():
    def scope(rel):
        return not rel.startswith(("..", "noncand/"))
    return Ctx(FIXTURE_DIR, scope, scope)


def init_worker(ctxs):
    CTX.update(ctxs)


def version_probe(_):
    return lib.libclang_version()


def comment_blocks(lines):
    """-> {line: (first, last, doxygen)} for every line inside a comment
    block (a `/* */` block, or a run of `//` lines of one kind). Crude
    lexing: string literals are not tracked."""
    out = {}
    i, n = 0, len(lines)
    while i < n:
        s = lines[i].strip()
        if s.startswith("//"):
            dox = s.startswith(("///", "//!")) and not s.startswith(("///<", "//!<", "////"))
            j = i
            while j + 1 < n:
                t = lines[j + 1].strip()
                if not t.startswith("//"):
                    break
                tdox = t.startswith(("///", "//!")) and not t.startswith(("///<", "//!<", "////"))
                if tdox != dox:
                    break
                j += 1
            for k in range(i, j + 1):
                out[k + 1] = (i + 1, j + 1, dox)
            i = j + 1
            continue
        pos = lines[i].find("/*")
        if pos >= 0 and lines[i][:pos].strip() == "":
            rest = lines[i][pos:]
            dox = (rest.startswith(("/**", "/*!")) and not rest.startswith(("/**/", "/**<", "/*!<")))
            j = i
            if "*/" not in rest[2:]:
                while j + 1 < n and "*/" not in lines[j + 1]:
                    j += 1
                j = min(j + 1, n - 1)
            for k in range(i, j + 1):
                out[k + 1] = (i + 1, j + 1, dox)
            i = j + 1
            continue
        i += 1
    return out


def directive_lines(lines):
    out, cont = set(), False
    for i, raw in enumerate(lines):
        s = raw.strip()
        if cont or s.startswith("#"):
            out.add(i + 1)
            cont = raw.rstrip().endswith("\\")
        else:
            cont = False
    return out


class Source:
    def __init__(self, path):
        try:
            with open(path, errors="replace") as fh:
                self.lines = fh.read().split("\n")
        except OSError:
            self.lines = []
        self._blocks = None
        self._directives = None

    @property
    def blocks(self):
        if self._blocks is None:
            self._blocks = comment_blocks(self.lines)
        return self._blocks

    @property
    def directives(self):
        if self._directives is None:
            self._directives = directive_lines(self.lines)
        return self._directives

    def text(self, first, last):
        return "\n".join(self.lines[first - 1:last])

    def line(self, n):
        return self.lines[n - 1] if 0 < n <= len(self.lines) else ""


MARKER = re.compile(r"//\s*complexity-ratchet:")


def plain_above(src, decl_line):
    """A plain comment block whose last line is the last non-blank line
    above the declaration line (a complexity-ratchet marker line is not
    part of the block)."""
    n = decl_line - 1
    while n > 0 and (not src.line(n).strip() or MARKER.search(src.line(n))):
        n -= 1
    return n > 0 and n in src.blocks


def blocking_token(src, start, attached_starts, limit=400):
    """Walk up from `start` over blank lines, comments, directives and
    lines ending in `;`, `{` or `}` to the nearest doxygen-form block.
    -> (block first line, token text, token line) when the block attaches
    to nothing in the TU and a token lies between it and the definition;
    else None."""
    token = None
    n = start - 1
    stop = max(0, start - limit)
    while n > stop:
        blk = src.blocks.get(n)
        if blk:
            first, _, dox = blk
            if dox:
                if first in attached_starts or token is None:
                    return None
                return first, token[0], token[1]
            n = first - 1
            continue
        s = src.line(n).strip()
        if not s:
            n -= 1
            continue
        if n in src.directives or s.endswith((";", "{", "}")):
            token = (s, n)
            n -= 1
            continue
        return None
    return None


def brief_paragraph(ci, block):
    """-> (text, broken) of a @brief block command's paragraph: the inline
    content joined (an inline command contributes its arguments), and whether
    a line break lies inside it (an inline child with a trailing newline
    followed by a non-whitespace child)."""
    L = ci.conf.lib
    para = L.clang_BlockCommandComment_getParagraph(block)
    parts, broken, pending_nl = [], False, False
    for i in range(L.clang_Comment_getNumChildren(para)):
        ch = L.clang_Comment_getChild(para, i)
        kind = L.clang_Comment_getKind(ch)
        if kind == K_TEXT:
            t = lib.cxstring(ci, L.clang_TextComment_getText(ch))
        elif kind == K_INLINE:
            t = " ".join(
                lib.cxstring(ci, L.clang_InlineCommandComment_getArgText(ch, j))
                for j in range(L.clang_InlineCommandComment_getNumArgs(ch)))
        else:
            t = ""
        if t.strip():
            if pending_nl:
                broken = True
            parts.append(" " + t if pending_nl else t)
        if L.clang_InlineContentComment_hasTrailingNewline(ch):
            pending_nl = True
    return " ".join("".join(parts).split()), broken


def brief_fault(text, broken):
    """Why a brief paragraph is not one sentence on one line, or None. The
    brief is the whole paragraph after @brief (the Doxyfile has AUTOBRIEF
    off); a detailed description starts after a blank comment line."""
    if broken:
        return "the @brief paragraph spans more than one line"
    if not text:
        return "the @brief paragraph is empty"
    code_free = CODE_SPAN.sub("", text).rstrip()
    bare = ABBREVIATIONS.sub(lambda m: m.group(0).replace(".", ""), code_free)
    if ABBREVIATIONS.search(code_free[-8:]) and code_free.endswith("."):
        bare += "."
    terms = TERMINATOR.findall(bare)
    if not re.search(r"[.!?]+[)\]\"']*$", bare):
        return "the @brief sentence has no terminator (`.`, `!`, `?`) at its end"
    if len(terms) > 1:
        return "the @brief paragraph holds %d sentences" % len(terms)
    return None


def parsed_comment_facts(ci, cursor):
    """-> (brief, group, params [(name, index valid)], has_return, fault) of
    the parsed comment attached to the cursor; fault is brief_fault() of the
    first @brief paragraph."""
    L = ci.conf.lib
    fc = L.clang_Cursor_getParsedComment(cursor)
    if L.clang_Comment_getKind(fc) != K_FULL:
        return False, False, [], False, None
    brief = group = has_return = False
    fault = None
    params = []
    for i in range(L.clang_Comment_getNumChildren(fc)):
        ch = L.clang_Comment_getChild(fc, i)
        kind = L.clang_Comment_getKind(ch)
        if kind in (K_BLOCK, K_VERBATIM_LINE):
            name = lib.cxstring(ci, L.clang_BlockCommandComment_getCommandName(ch))
            if kind == K_VERBATIM_LINE or name in GROUP_CMDS:
                group = group or name in GROUP_CMDS
            if name in BRIEF_CMDS and not brief:
                brief = True
                fault = brief_fault(*brief_paragraph(ci, ch))
            if name in RETURN_CMDS:
                has_return = True
        elif kind == K_PARAM:
            params.append((
                lib.cxstring(ci, L.clang_ParamCommandComment_getParamName(ch)),
                bool(L.clang_ParamCommandComment_isParamIndexValid(ch))))
    return brief, group, params, has_return, fault


def score_entry(task):
    """Parse one database entry. -> dict(tu, errors, wdoc, defs, hdecls)."""
    ctxname, tu_rel, src_abs, flags, cwd = task
    ctx = CTX[ctxname]
    root = ctx.root
    ci = lib.libclang()
    L = ci.conf.lib
    K = ci.CursorKind
    t0 = time.time()
    os.chdir(cwd)
    tu = ci.Index.create().parse(src_abs, flags + DOC_FLAGS)

    relcache = {}

    def rel(name):
        if name not in relcache:
            relcache[name] = os.path.relpath(
                os.path.normpath(os.path.join(cwd, name)), root)
        return relcache[name]

    def cfile(c):
        f = c.location.file
        return rel(f.name) if f else None

    errors, wdoc = [], []
    for d in tu.diagnostics:
        f = rel(d.location.file.name) if d.location.file else None
        if d.severity >= 3:
            errors.append((f, d.location.line, d.spelling))
        elif (d.option or "").startswith("-Wdocumentation") and f \
                and not f.startswith("..") and ctx.candidate(f):
            wdoc.append((f, d.location.line, d.spelling))

    cursors = lib.definition_cursors(tu)
    files = [cfile(c) for c in cursors]
    by_usr = defaultdict(list)
    hdecls = defaultdict(set)
    for c, f in zip(cursors, files):
        if f is None or f.startswith(".."):
            continue
        is_def = c.is_definition()
        if not is_def and is_header(f) and ctx.candidate(f):
            hdecls[c.spelling].add(f)
        by_usr[c.get_usr()].append((c, f, is_def))

    sources = {}

    def source(f):
        if f not in sources:
            sources[f] = Source(os.path.join(root, f))
        return sources[f]

    attached = {}

    def attached_starts(f):
        """Start lines of every comment range attached to a declaration in
        file f (functions from the walk, and the TU's top-level cursors)."""
        if f not in attached:
            out = set()
            for c, cf in zip(cursors, files):
                if cf == f:
                    cr = L.clang_Cursor_getCommentRange(c)
                    if cr.start.file:
                        out.add(cr.start.line)
            for c in tu.cursor.get_children():
                if c.location.file and rel(c.location.file.name) == f:
                    cr = L.clang_Cursor_getCommentRange(c)
                    if cr.start.file:
                        out.add(cr.start.line)
            attached[f] = out
        return attached[f]

    defs = []
    wdoc_by_file = defaultdict(list)
    for f, line, msg in wdoc:
        wdoc_by_file[f].append((line, msg))
    for usr, group in by_usr.items():
        for c, f, is_def in group:
            if not is_def or not ctx.scope(f):
                continue
            if c.kind != K.FUNCTION_DECL and c.is_default_method():
                continue
            defs.append(classify(ci, ctx, c, f, group, source, attached_starts,
                                 wdoc_by_file))
    return dict(tu=tu_rel, errors=errors, wdoc=wdoc, defs=defs,
                hdecls={k: sorted(v) for k, v in hdecls.items()},
                seconds=time.time() - t0)


def classify(ci, ctx, c, f, group, source, attached_starts, wdoc_by_file):
    L = ci.conf.lib
    ext = c.extent
    start, end = ext.start.line, ext.end.line
    src = source(f)
    macro = "{" not in src.text(start, end)
    redecls = []
    for r, rf, r_def in group:
        cr = L.clang_Cursor_getCommentRange(r)
        rng = None
        if cr.start.file:
            rng = (os.path.relpath(os.path.normpath(os.path.join(
                os.getcwd(), cr.start.file.name)), ctx.root),
                cr.start.line, cr.end.line)
        redecls.append(dict(cursor=r, file=rf, is_def=r_def,
                            start=r.extent.start.line, end=r.extent.end.line,
                            line=r.location.line, rng=rng))
    me = next(r for r in redecls if r["cursor"] == c)
    if is_header(f):
        canon, headers = me, []
    else:
        hdr = sorted((r for r in redecls if not r["is_def"] and is_header(r["file"])
                      and ctx.candidate(r["file"])),
                     key=lambda r: (r["file"], r["start"]))
        headers = sorted({r["file"] for r in hdr})
        canon = hdr[0] if hdr else me
    noncand = sorted({r["file"] for r in redecls if not r["is_def"]
                      and is_header(r["file"]) and not ctx.candidate(r["file"])})
    ranges = sorted({r["rng"] for r in redecls if r["rng"]})
    facts = dict(
        file=f, name=c.spelling, start=start, end=end, macro=macro,
        linkage=str(c.linkage).rsplit(".", 1)[-1],
        void=c.result_type.kind == ci.TypeKind.VOID,
        canonical=(canon["file"], canon["start"]),
        canonical_line=canon["line"],
        canonical_is_header=canon is not me,
        two_headers=headers if len(headers) > 1 else [],
        noncand=noncand,
        extents=[(f, start, end)] + ([(canon["file"], canon["start"], canon["end"])]
                                     if canon is not me else []),
        ranges=ranges, blocking=None, missing=[], needs_return=False,
        wdoc=[], group=False, brief=False)
    params = [a.spelling for a in canon["cursor"].get_arguments()]
    facts["params"] = [p for p in params if p]
    if macro:
        facts["form"] = "MACRO"
        return facts
    if len(ranges) > 1:
        facts["form"] = "duplicate"
        return facts
    if not ranges:
        csrc = source(canon["file"])
        facts["form"] = "plain" if plain_above(csrc, canon["start"]) else "none"
        for site_file, site_start in ((canon["file"], canon["start"]), (f, start)):
            b = blocking_token(source(site_file), site_start,
                               attached_starts(site_file))
            if b:
                facts["blocking"] = (site_file,) + b
                break
            if site_file == f and site_start == start:
                break
        return facts
    rng = ranges[0]
    owner = min((r for r in redecls if r["file"] == rng[0] and r["start"] >= rng[2]),
                key=lambda r: r["start"], default=None)
    if owner is None or (owner["file"], owner["start"]) != facts["canonical"]:
        facts["form"] = "misplaced"
        facts["owner"] = (owner["file"], owner["start"]) if owner else (rng[0], rng[1])
        facts["comment"] = rng
        return facts
    facts["comment"] = rng
    brief, grp, pcmds, has_ret, fault = parsed_comment_facts(ci, canon["cursor"])
    raw = source(rng[0]).text(rng[1], rng[2])
    grp = grp or bool(re.search(r"[@\\]\{", raw))
    facts["brief"], facts["group"] = brief, grp
    facts["wdoc"] = [(ln, m) for ln, m in wdoc_by_file.get(rng[0], ())
                     if rng[1] <= ln <= rng[2]]
    if not brief or grp:
        facts["form"] = "malformed"
        return facts
    facts["brief_fault"] = fault
    if fault:
        facts["form"] = "longbrief"
        return facts
    if facts["linkage"] == "EXTERNAL":
        named = facts["params"]
        valid = [n for n, ok in pcmds if ok]
        facts["missing"] = [n for n in named if n not in valid]
        facts["needs_return"] = not facts["void"] and not has_ret
        if (not all(ok for _, ok in pcmds) or len(pcmds) != len(named)
                or facts["needs_return"]):
            facts["form"] = "untagged"
            return facts
    facts["form"] = "malformed" if facts["wdoc"] else "meets"
    return facts


class Corpus:
    def __init__(self):
        self.facts = {}        # key -> merged facts
        self.tus_of = {}       # key -> set of TU relpaths
        self.errors = {}       # TU relpath -> [(file, line, message)]
        self.wdoc = set()      # (file, line, message)
        self.hdecls = defaultdict(set)
        self.scored = 0
        self.definitions = 0
        self.seconds = 0.0


def merge_results(results):
    """Keys per (file, definition line, name) at the worst form across
    entries; a repeated name within one file takes an ordinal by line."""
    corpus = Corpus()
    sites = {}
    tus = defaultdict(set)
    for res in results:
        corpus.scored += 1
        corpus.seconds += res["seconds"]
        if res["errors"]:
            corpus.errors.setdefault(res["tu"], []).extend(res["errors"])
        corpus.wdoc.update(tuple(w) for w in res["wdoc"])
        for name, hs in res["hdecls"].items():
            corpus.hdecls[name].update(hs)
        for fx in res["defs"]:
            site = (fx["file"], fx["start"], fx["name"])
            tus[site].add(res["tu"])
            old = sites.get(site)
            if old is None or rank(fx) > rank(old):
                sites[site] = fx
    corpus.definitions = len(sites)
    by_name = defaultdict(list)
    for (f, line, name) in sites:
        by_name[(f, name)].append(line)
    for (f, name), lines in by_name.items():
        for n, line in enumerate(sorted(lines)):
            key = "%s:%s" % (f, name if n == 0 else "%s#%d" % (name, n + 1))
            corpus.facts[key] = sites[(f, line, name)]
            corpus.tus_of[key] = tus[(f, line, name)]
    return corpus


def rank(fx):
    if fx["form"] == "MACRO":
        return -1
    return FORM_RANK[fx["form"]] + (10 if fx.get("two_headers") else 0)


class Scorer:
    """The process pool. libclang loads in the workers only."""

    def __init__(self, ctxs):
        self.pool = multiprocessing.get_context("fork").Pool(
            JOBS, initializer=init_worker, initargs=(ctxs,))

    def version(self):
        return self.pool.apply(version_probe, (None,))

    def score(self, tasks):
        return self.pool.map(score_entry, tasks, chunksize=1)

    def close(self):
        self.pool.close()
        self.pool.join()


def check_version(scorer):
    v = scorer.version()
    m = re.search(r"version (\d+)\.", v)
    lib_major = int(m.group(1)) if m else None
    clang = shutil.which("clang-%d" % LIB_MAJOR) or shutil.which("clang")
    cm = None
    if clang:
        out = subprocess.run([clang, "--version"], capture_output=True,
                             text=True).stdout
        mm = re.search(r"version (\d+)\.", out)
        cm = int(mm.group(1)) if mm else None
    if lib_major != LIB_MAJOR or cm != LIB_MAJOR:
        raise Refusal("libclang reports %r and clang --version major %s; the "
                      "baseline is pinned to %d. Install libclang1-%d and "
                      "clang-%d, or recapture under the new version: set "
                      "LIB_MAJOR and run `doc_ratchet.py baseline`."
                      % (v, cm, LIB_MAJOR, LIB_MAJOR, LIB_MAJOR))
    return "libclang %d" % LIB_MAJOR


def entry_tasks(root, entries, files):
    tasks = []
    for rel in sorted(files):
        for e in entries.get(rel, ()):
            tasks.append(("repo", rel, os.path.join(root, rel),
                          lib.entry_flags(e), e["directory"]))
    return tasks


# --------------------------------------------------------------------------
# Negative controls: fixture TUs under tests/doc_fixture/, evaluated before
# every hook and check verdict.
# --------------------------------------------------------------------------
def run_controls(scorer):
    with open(os.path.join(FIXTURE_DIR, "controls.json")) as fh:
        controls = json.load(fh)
    tus = sorted({tu for c in controls for tu in c["tus"]})
    tasks = []
    for tu in tus:
        flags = (["-x", "c++", "-std=c++17"] if tu.endswith(".cpp")
                 else ["-std=c11"]) + ["-I" + FIXTURE_DIR]
        tasks.append(("fixture", tu, os.path.join(FIXTURE_DIR, tu), flags,
                      FIXTURE_DIR))
    results = scorer.score(tasks)
    by_tu = {r["tu"]: r for r in results}
    failures = []
    for c in controls:
        corpus = merge_results([by_tu[t] for t in c["tus"]])
        if any(corpus.errors.values()):
            failures.append("%s: fixture errors %s" % (c["name"], corpus.errors))
            continue
        facts = corpus.facts
        for key in c.get("present", []):
            if key not in facts:
                failures.append("%s: %s not found by the walk" % (c["name"], key))
        acted = set(c["acted"])
        rows = {k: (v, "") for k, v in c.get("rows", {}).items()}
        changed = {f: [tuple(h) for h in hs] for f, hs in c.get("changed", {}).items()}
        wdoc = sorted(corpus.wdoc)
        files = {key_file(k) for k in facts} | set(c["tus"])
        verdict, new_rows, bins = decide(
            rows, facts, acted, {}, changed, files, files, None,
            staged_files=c.get("staged", []), wdoc=wdoc)
        got = {}
        for k, code, _ in bins["REFUSED"]:
            got[k] = code
        for k, _, why in bins["RETIRED"]:
            got.setdefault(k, "retired")
        for k in bins["MACRO"]:
            got.setdefault(k, "MACRO")
        for k in acted:
            if k in new_rows and k not in got:
                got[k] = "kept"
            got.setdefault(k, "pass")
        if bins["WDOC"]:
            got["WDOC"] = "bin"
        for k, want in c["expect"].items():
            if got.get(k) != want:
                failures.append("%s: %s is %s, expected %s" % (
                    c["name"], k, got.get(k), want))
        if verdict != c["verdict"]:
            failures.append("%s: verdict %s, expected %s" % (
                c["name"], verdict, c["verdict"]))
        for k, want in c.get("forms", {}).items():
            if facts.get(k, {}).get("form") != want:
                failures.append("%s: %s form %s, expected %s" % (
                    c["name"], k, facts.get(k, {}).get("form"), want))
        if "wdoc_count" in c and len(bins["WDOC"]) != c["wdoc_count"]:
            failures.append("%s: WDOC %s, expected %d entries" % (
                c["name"], bins["WDOC"], c["wdoc_count"]))
        for k, want in c.get("block", {}).items():
            b = facts.get(k, {}).get("blocking")
            if not b or want not in b[2]:
                failures.append("%s: %s blocking token %s, expected %r" % (
                    c["name"], k, b, want))
    if failures:
        raise Refusal("negative controls FAILED, no verdict is trusted:\n  "
                      + "\n  ".join(failures))
    return len(controls)


# --------------------------------------------------------------------------
# Reports
# --------------------------------------------------------------------------
def fmt_ranges(ranges):
    out = []
    for a, b in ranges:
        out.append("%d" % a if a == b else "%d-%d" % (a, b))
    return ", ".join(out)


def skeleton(fx, root):
    ext = fx["linkage"] == "EXTERNAL"
    lines = []
    if ext:
        lines += ["@param %s <meaning>" % p for p in fx["params"]]
        if not fx["void"]:
            lines.append("@return <meaning>")
    text = ["/** @brief <what it does, one sentence>.", " *",
            " * <detail, when there is more to say>"]
    text += [" * " + l for l in lines]
    text[-1] += " */"
    src = Source(os.path.join(root, fx["canonical"][0]))
    sig = src.line(fx["canonical"][1]).rstrip()
    return text + [sig]


def requirement(fx):
    if fx["linkage"] != "EXTERNAL":
        return "%s linkage: a @brief is enough." % fx["linkage"].capitalize()
    need = ["@brief"] + ["@param %s" % p for p in fx["params"]]
    if not fx["void"]:
        need.append("@return")
    return "External linkage: %s." % ", ".join(need)


def explain(key, code, fx, root, rows, changed):
    """Refusal lines for one key."""
    out = []
    if code == "unwired":
        return ["doc-ratchet: REFUSED  %s  UNWIRED: %s is on disk and no target "
                "compiles it. Wire it into CMake and re-run `%s`, or delete it."
                % (key, key_file(key), lib.CONFIGURE)]
    if code == "wdoc":
        return ["doc-ratchet: REFUSED  %s  -Wdocumentation: %s" % (key, fx)]
    f, start, end = fx["file"], fx["start"], fx["end"]
    status = ""
    if code == "clause2":
        hit = [(s, s + c - 1) for s, c in changed.get(f, ()) if c and s <= end
               and s + c - 1 >= start]
        status = "touched, lines %d-%d; changed %s" % (start, end, fmt_ranges(hit))
    elif code == "clause1":
        added = [(s, s + c - 1) for s, c in changed.get(f, ()) if c]
        new = all(any(a <= n <= b for a, b in added) for n in range(start, end + 1))
        status = "new" if new else "no row"
    else:
        status = code
    out.append("doc-ratchet: REFUSED  %s  (%s)" % (key, status))
    site = "%s:%d" % (fx["canonical"][0], fx["canonical_line"])
    where = ("the declaration" if fx["canonical_is_header"] else "the definition")
    form = fx["form"]
    if code == "two-headers":
        out.append("  declared in two project headers in this TU: %s. Remove the "
                   "declaration from the header that is not the function's "
                   "interface." % ", ".join(fx["two_headers"]))
        return out
    if code == "duplicate":
        out.append("  doxygen comments at %s; keep one, at %s (%s)." % (
            ", ".join("%s:%d" % (r[0], r[1]) for r in fx["ranges"]), site, where))
        return out
    if form == "misplaced":
        c = fx.get("comment") or ("?", 0, 0)
        out.append("  the doxygen comment at %s:%d attaches to %s:%d; the "
                   "canonical site is %s (%s). Move it there." % (
                       c[0], c[1], fx["owner"][0], fx["owner"][1], site, where))
        return out
    if fx.get("blocking"):
        bf, first, tok, tline = fx["blocking"]
        out.append("  /** */ at %d does not attach: `%s` at %d sits between the "
                   "comment and the definition; move the comment below it."
                   % (first, tok[:60], tline))
        return out
    if form in ("none", "plain"):
        if fx["canonical_is_header"]:
            out.append("  canonical site is %s (%s)." % (site, where))
        out.append("  %s %s" % (
            "no doxygen comment attaches." if form == "none" else
            "the comment above the site is plain (`/*`, `//`); doxygen form "
            "is `/**` or `///`.", requirement(fx)))
        out += ["    " + l for l in skeleton(fx, root)]
        return out
    if form == "malformed":
        if fx.get("wdoc"):
            for ln, msg in fx["wdoc"]:
                out.append("  -Wdocumentation at %d: %s" % (ln, msg))
        elif fx.get("group"):
            out.append("  the comment is a group header (@name, @defgroup, "
                       "@addtogroup, @ingroup, @{); the function needs its own "
                       "comment with a @brief.")
        else:
            out.append("  the comment has no @brief.")
        return out
    if form == "longbrief":
        out.append("  %s. With AUTOBRIEF off the whole paragraph after @brief "
                   "is the brief: write one sentence on the @brief line, a "
                   "blank comment line, then the detail." % fx["brief_fault"])
        out += ["    " + l for l in (
            "/** @brief <one sentence>.", " *", " * <detail>",
            " * @param ... */")]
        return out
    if form == "untagged":
        lacks = ["@param %s" % p for p in fx["missing"]]
        if fx["needs_return"]:
            lacks.append("@return")
        out.append("  %s The comment at %s lacks %s." % (
            requirement(fx), site, ", ".join(lacks) or
            "one @param per named parameter of the declaration there"))
        return out
    return out


REFUSAL_OPTIONS = """
Ways to land this commit:
  1. document   add the comment at the site named above: what the function
                does, the invariant it holds, the problem it solves; never
                its history.
  2. move       a comment at the wrong declaration, or above a blocking
                token, moves to the site named above.
  3. accept     %s="<reason>" git commit ...
                for a mechanical pass (a reformat, a move between TUs)
                where documenting every touched function is out of scope.
  4. fence      a mis-read is a defect in the fence: fix or retire it this
                session.
""" % ACCEPT_VAR


def print_refusals(bins, facts, root, rows, changed):
    for key, code, detail in bins["REFUSED"]:
        fx = facts.get(key) if code != "wdoc" else detail
        if fx is None and code not in ("wdoc", "unwired"):
            continue
        for line in explain(key, code, fx, root, rows, changed):
            say(line)
    if bins["REFUSED"]:
        say(REFUSAL_OPTIONS)


def row_counts(rows):
    c = defaultdict(int)
    for form, _ in rows.values():
        c[form] += 1
    return "rows %d (%s)" % (len(rows), ", ".join(
        "%s %d" % (f, c[f]) for f in FORMS))


# --------------------------------------------------------------------------
# Corpus inputs shared by hook, check, tighten, baseline
# --------------------------------------------------------------------------
def start_scorer(root):
    scorer = Scorer({"repo": repo_ctx(root), "fixture": fixture_ctx()})
    try:
        version = check_version(scorer)
        controls = run_controls(scorer)
    except BaseException:
        scorer.close()
        raise
    return scorer, version, controls


def no_header_include(corpus):
    """EXTERNAL definitions with no project-header declaration in their TU
    whose name a project header declares elsewhere."""
    out = []
    for key, fx in sorted(corpus.facts.items()):
        if (fx["linkage"] == "EXTERNAL" and not fx["canonical_is_header"]
                and not is_header(fx["file"]) and fx["form"] != "MACRO"
                and corpus.hdecls.get(fx["name"])):
            out.append("%s  declared in %s" % (
                key, ", ".join(sorted(corpus.hdecls[fx["name"]]))))
    return out


def check_carveouts(root, corpus):
    """The Windows-only list against the tree: each file holds the name and
    the linux-release parse does not see a definition of it."""
    if os.path.realpath(root) != os.path.realpath(lib.OWN_ROOT):
        return
    for key in WINDOWS_ONLY:
        f, name = key.rsplit(":", 1)
        try:
            with open(os.path.join(root, f), errors="replace") as fh:
                ok = re.search(r"\b%s\s*\(" % re.escape(name), fh.read())
        except OSError:
            ok = None
        if not ok or key in corpus.facts:
            raise Refusal("WINDOWS_ONLY is stale: %s is %s. Edit WINDOWS_ONLY in "
                          "scripts/fences/doc_ratchet.py." % (
                              key, "parsed under linux-release" if ok
                              else "not in the file"))


def header_current(text):
    """Whether the header's `requirement:` line is REQUIREMENT_LINE. A
    baseline written under an older line is accepted and rewritten by the
    next hook or tighten write."""
    return lib.parse_header(text).get("requirement") == REQUIREMENT_LINE


def library_line(text):
    return lib.parse_header(text).get("library") if text else None


def validate_source(root, head, rows, facts, version):
    sha = lib.head_sha(root)
    htext = lib.head_tsv(root, TSV)
    hrows = parse_tsv(htext)[1] if htext is not None else {}
    by = head.get("written-by")
    if by == "baseline":
        if htext is not None and library_line(htext) == head.get("library"):
            raise Refusal("staged %s is a `baseline` recapture, and HEAD already "
                          "carries a baseline under the same library. Use "
                          "`tighten`." % TSV)
        return
    if head.get("head") != sha:
        raise Refusal("staged %s was computed against %s, HEAD is %s. Re-run "
                      "`doc_ratchet.py tighten`, stage it, commit."
                      % (TSV, head.get("head", "?")[:10], sha[:10]))
    for key, (form, reason) in rows.items():
        if key not in hrows and not reason:
            raise Refusal("staged %s adds a row for %s with no `accepted` "
                          "reason: a hand edit." % (TSV, key))


# --------------------------------------------------------------------------
# hook
# --------------------------------------------------------------------------
def candidate_headers(root, paths):
    tracked_ctx = repo_ctx(root)
    return sorted(p for p in paths if p and is_header(p)
                  and p.endswith((".h", ".hpp")) and tracked_ctx.candidate(p))


def mode_hook(root):
    t0 = time.time()
    lib.refuse_temporary_index()
    changes, base = lib.staged_changes(root)
    scope = lib.scope_changes(changes, FENCE.in_scope)
    staged_paths = {b or a for s, a, b in changes if s != "D"}
    staged_hdrs = candidate_headers(root, staged_paths)
    staged_text = lib.staged_tsv(root, TSV, changes)
    shead, srows = parse_tsv(staged_text) if staged_text is not None else ({}, {})

    if not scope and not staged_hdrs:
        if not lib.reset_stale_hook_tsv(root, FENCE, staged_text) \
                and staged_text is not None:
            validate_source(root, shead, srows, {}, None)
        return 0

    db_path, entries = lib.load_db(root, FENCE)
    if entries is None:
        raise Refusal("no compile database at %s. Run: %s" % (db_path, lib.CONFIGURE))
    scorer, version, controls = start_scorer(root)
    try:
        lib.check_unscored(root, entries, FENCE)
        renames, acted_files, need_entry, moved = lib.acted_on_paths(root, base, scope)
        for rel in need_entry:
            if is_header(rel) or rel in lib.UNSCORED:
                continue
            if rel not in entries:
                raise Refusal("%s has no exact compile-database entry. Run: %s"
                              % (rel, lib.CONFIGURE))
        full = bool(staged_hdrs) or any(is_header(f) for f in acted_files)
        identical = {new for new, same in renames.values() if same}
        parse_files = ([f for f in entries if os.path.isfile(os.path.join(root, f))
                        and f not in moved] if full else
                       [f for f in acted_files if f in entries and f not in identical])
        corpus = merge_results(scorer.score(entry_tasks(root, entries, parse_files)))
    finally:
        scorer.close()

    worktree_dirty = set(git(root, "diff", "--name-only").stdout.split("\n"))
    head_dirty = lib.dirty_paths(root, FENCE) | set(
        git(root, "diff", "--name-only", "HEAD").stdout.split("\n"))
    bins = defaultdict(list)
    for tu, errs in sorted(corpus.errors.items()):
        if tu in acted_files or tu not in head_dirty:
            raise Refusal("%s does not parse clean; functions after an error "
                          "would read as retirements:\n  %s" % (tu, "\n  ".join(
                              "%s:%s: %s" % e for e in errs[:5])))
        bins["DIRTY"].append("%s (error in a TU dirty against HEAD)" % tu)
    dirty = lib.unstaged_refusal(root, acted_files, TSV)

    # The acted-on keys: the functions of staged scope paths, and every
    # function whose canonical site is a staged header.
    acted = set()
    for key, fx in corpus.facts.items():
        if key_file(key) in acted_files:
            acted.add(key)
        elif fx["canonical"][0] in staged_hdrs:
            if key_file(key) in worktree_dirty or \
                    corpus.tus_of[key] & worktree_dirty:
                bins["DIRTY"].append(key)
            else:
                acted.add(key)
    # Rows of acted files whose function is gone retire; carried keys first.
    if staged_text is not None and shead.get("written-by") in ("tighten", "baseline"):
        validate_source(root, shead, srows, corpus.facts, version)
        source_name, source_text, before = ("staged (%s)" % shead["written-by"],
                                            staged_text, srows)
    else:
        htext = lib.head_tsv(root, TSV)
        if htext is None:
            raise Refusal("no baseline in HEAD or the index. Run "
                          "`doc_ratchet.py baseline`, stage %s." % TSV)
        source_name, source_text, before = "HEAD", htext, parse_tsv(htext)[1]
        if staged_text is not None:
            source_name += " (staged hook-written file discarded)"
        if library_line(htext) != version:
            raise Refusal("HEAD's %s was captured under %s, this is %s. Run "
                          "`doc_ratchet.py baseline`." % (TSV, library_line(htext),
                                                          version))
    for key in before:
        if key_file(lib.carry_key(key, renames)) in acted_files:
            acted.add(lib.carry_key(key, renames))

    # A canonical-site header comment the index does not carry would retire
    # a row with no comment in the commit.
    hdr_hunks = lib.diff_hunks(root, base, sorted(
        {corpus.facts[k]["canonical"][0] for k in acted if k in corpus.facts
         and corpus.facts[k]["canonical_is_header"]} & worktree_dirty),
        cached=False)
    for key in sorted(acted):
        fx = corpus.facts.get(key)
        if not fx or not fx["canonical_is_header"]:
            continue
        hf, hstart = fx["canonical"]
        lo = fx["comment"][1] if fx.get("comment") and fx["comment"][0] == hf else hstart
        for s, c in hdr_hunks.get(hf, ()):
            if (c and s <= hstart and s + c - 1 >= lo) or (not c and lo <= s <= hstart):
                raise Refusal("%s has unstaged changes over the declaration of %s "
                              "(lines %d-%d); the commit would not carry them. "
                              "`git add -p %s`." % (hf, key, lo, hstart, hf))
    bins["DIRTY-HEADER"] = sorted(f for f in worktree_dirty if f and is_header(f)
                                  and f not in staged_paths
                                  and any(corpus.facts[k]["canonical"][0] == f
                                          for k in acted if k in corpus.facts))

    present = lib.present_scope_files(root, FENCE) | {
        f for f in acted_files if os.path.isfile(os.path.join(root, f))}
    changed = lib.diff_hunks(root, base, sorted(
        {f for f in acted_files | set(staged_hdrs)
         if os.path.isfile(os.path.join(root, f))}))
    accept = lib.accept_reason(ACCEPT_VAR)
    wdoc = sorted(w for w in corpus.wdoc)
    verdict, new_rows, dbins = decide(
        before, corpus.facts, acted, renames, changed,
        lib.wired(entries, present), present, accept,
        staged_files=staged_paths, wdoc=wdoc)
    bins.update(dbins)
    say("doc-ratchet: %d entries parsed (%s), %d definitions, %d acted on, %d "
        "controls; baseline source: %s; %d accepted rows; %.1f s" % (
            corpus.scored, "full corpus" if full else "staged TUs",
            corpus.definitions, len(acted), controls, source_name,
            sum(1 for v in new_rows.values() if v[1]), time.time() - t0))
    lib.print_bins({k: v for k, v in bins.items()
                    if k in ("RETIRED", "FORM", "ACCEPTED", "WDOC", "DIRTY",
                             "DIRTY-HEADER")}, quiet=("RETIRED", "FORM"))
    if verdict == "REFUSED":
        print_refusals(bins, corpus.facts, root, before, changed)
        return 1
    for key, row, form in bins["ACCEPTED"]:
        say("doc-ratchet: ACCEPTED  %s  %s  (%s)" % (key, form, accept))
    if new_rows == before and library_line(source_text) == version and \
            header_current(source_text):
        text = source_text
    else:
        text = render_tsv(new_rows, "hook", lib.head_sha(root), version)
    lib.write_staged(root, TSV, text, staged_text)
    say("doc-ratchet: PASS")
    return 0


# --------------------------------------------------------------------------
# check / tighten / baseline
# --------------------------------------------------------------------------
def corpus_inputs(root, ctest=False):
    db_path, entries = lib.load_db(root, FENCE)
    if entries is None:
        say("doc-ratchet: SKIP, no compile database at %s" % db_path)
        sys.exit(64 if ctest else 0)
    scorer, version, controls = start_scorer(root)
    try:
        lib.check_unscored(root, entries, FENCE, complete=True)
        files = [f for f in entries if os.path.isfile(os.path.join(root, f))]
        corpus = merge_results(scorer.score(entry_tasks(root, entries, files)))
    finally:
        scorer.close()
    if corpus.scored == 0 or corpus.definitions == 0:
        raise Refusal("VACUOUS: %d TUs parsed, %d definitions. No verdict."
                      % (corpus.scored, corpus.definitions))
    present = lib.present_scope_files(root, FENCE)
    dirty = {f for f in lib.dirty_paths(root, FENCE) if f}
    stale = sorted(f for f in entries if not os.path.isfile(os.path.join(root, f)))
    return entries, version, controls, corpus, present, dirty, stale


def clean_keys(corpus, dirty):
    clean, dirty_keys, dirty_hdr = set(), [], set()
    for key, fx in corpus.facts.items():
        tus = corpus.tus_of[key]
        if key_file(key) in dirty or tus & dirty or tus & set(corpus.errors):
            dirty_keys.append(key)
            continue
        clean.add(key)
        if fx["canonical"][0] in dirty:
            dirty_hdr.add(fx["canonical"][0])
    return clean, sorted(dirty_keys), sorted(dirty_hdr)


def full_decide(root, entries, corpus, present, dirty, before, clean):
    # Rows whose function is absent from a clean TU (or whose file left the
    # tree) join the acted-on set and retire.
    acted = set(clean)
    parsed_clean = {key_file(k) for k in clean}
    for key in before:
        f = key_file(key)
        if key not in corpus.facts and f not in dirty and (
                not os.path.exists(os.path.join(root, f)) or f in parsed_clean):
            acted.add(key)
    return decide(before, corpus.facts, acted, {}, {},
                  lib.wired(entries, present), present, None,
                  wdoc=sorted(corpus.wdoc), unwired_all=True)


def list_population(root, corpus, before, bins, dirty_keys, nhi):
    out = sys.stdout
    refused = {k: code for k, code, _ in bins["REFUSED"]}
    dirty_set = set(dirty_keys)
    for key in sorted(corpus.facts):
        fx = corpus.facts[key]
        form = fx["form"]
        if key in dirty_set:
            b = "DIRTY"
        elif form == "MACRO":
            b = "MACRO"
        elif key in refused:
            b = "REFUSED:" + refused[key]
        elif key in before:
            b = "row"
        else:
            b = "meets" if form == "meets" else "no-row"
        out.write("%s\t%s\t%s\t%s\n" % (key, b, form.lower() if form != "MACRO"
                                        else "MACRO", fx["linkage"]))
    for line in nhi:
        out.write("NO-HEADER-INCLUDE\t%s\n" % line)
    for key in WINDOWS_ONLY:
        out.write("CARVE-OUT\t%s\twindows-only\n" % key)


def mode_check(root, argv):
    t0 = time.time()
    ctest, listing = "--ctest" in argv, "--list" in argv
    entries, version, controls, corpus, present, dirty, stale = \
        corpus_inputs(root, ctest)
    htext = lib.head_tsv(root, TSV)
    if htext is None:
        raise Refusal("HEAD carries no %s. Run `doc_ratchet.py baseline` and "
                      "commit it." % TSV)
    if library_line(htext) != version:
        raise Refusal("HEAD's %s was captured under %s, this is %s. Run "
                      "`doc_ratchet.py baseline`." % (TSV, library_line(htext),
                                                      version))
    before = parse_tsv(htext)[1]
    check_carveouts(root, corpus)
    clean, dirty_keys, dirty_hdr = clean_keys(corpus, dirty)
    verdict, new_rows, bins = full_decide(root, entries, corpus, present,
                                          dirty, before, clean)
    clean_tus = [f for f in entries if f not in dirty and f not in stale]
    errors = sorted(tu for tu in corpus.errors if tu not in dirty)
    nhi = no_header_include(corpus)
    filtered = sum(1 for fx in corpus.facts.values() if fx["noncand"])
    macro = sum(1 for fx in corpus.facts.values() if fx["form"] == "MACRO")
    meets = sum(1 for fx in corpus.facts.values() if fx["form"] == "meets")
    say("doc-ratchet: engaged %d/%d TUs clean against HEAD, %d definitions "
        "(%d meet), %d controls, %s, MACRO %d, candidates-filtered %d, "
        "accepted rows %d; %.1f s" % (
            len(clean_tus), len(entries), corpus.definitions, meets, controls,
            row_counts(before), macro, filtered,
            sum(1 for v in before.values() if v[1]), time.time() - t0))
    if listing:
        list_population(root, corpus, before, bins, dirty_keys, nhi)
    fail_bins = {"ERROR": ["%s: %s" % (tu, "%s:%s: %s" % corpus.errors[tu][0])
                           for tu in errors],
                 "WDOC": bins["WDOC"]}
    report = {"RETIRABLE": [k for k, _, _ in bins["RETIRED"]],
              "FORM": bins["FORM"], "DIRTY": dirty_keys,
              "DIRTY-HEADER": dirty_hdr, "STALE-DB": stale,
              "NO-HEADER-INCLUDE": nhi}
    lib.print_bins(dict(report, **fail_bins),
                   quiet=("DIRTY", "RETIRABLE", "FORM"))
    if report["RETIRABLE"] or report["FORM"]:
        say("  `doc_ratchet.py tighten` writes the RETIRABLE and FORM bins.")
    if not clean_tus:
        raise Refusal("VACUOUS: no TU is clean against HEAD.")
    if verdict == "REFUSED" or any(fail_bins.values()):
        print_refusals(bins, corpus.facts, root, before, {})
        say("A function reached HEAD undocumented without the hook (merge, "
            "revert, --no-verify). Repair: document it, or commit it through "
            "the hook with %s." % ACCEPT_VAR)
        return 1
    say("doc-ratchet: PASS")
    return 0


def mode_tighten(root):
    entries, version, controls, corpus, present, dirty, stale = corpus_inputs(root)
    htext = lib.head_tsv(root, TSV)
    if htext is None:
        raise Refusal("HEAD carries no %s. Run `baseline`." % TSV)
    before = parse_tsv(htext)[1]
    clean, dirty_keys, _ = clean_keys(corpus, dirty)
    verdict, new_rows, bins = full_decide(root, entries, corpus, present,
                                          dirty, before, clean)
    lib.print_bins({"RETIRED": bins["RETIRED"], "FORM": bins["FORM"],
                    "DIRTY": dirty_keys}, quiet=("DIRTY",))
    if new_rows == before and header_current(htext):
        say("doc-ratchet: %s is current (%d rows); nothing written."
            % (TSV, len(new_rows)))
    else:
        with open(os.path.join(root, TSV), "w") as fh:
            fh.write(render_tsv(new_rows, "tighten", lib.head_sha(root), version))
        say("doc-ratchet: wrote %s (%s). Stage and commit it."
            % (TSV, row_counts(new_rows)))
    if verdict == "REFUSED":
        print_refusals(bins, corpus.facts, root, before, {})
        return 1
    return 0


def mode_baseline(root):
    entries, version, controls, corpus, present, dirty, stale = corpus_inputs(root)
    bad = sorted(f for f in dirty if f != TSV)
    if bad:
        raise Refusal("baseline refuses dirty scored files (the rows would "
                      "describe uncommitted content): %s" % ", ".join(bad[:10]))
    if corpus.errors:
        raise Refusal("TUs with errors: %s" % ", ".join(sorted(corpus.errors)))
    rows = {}
    for key, fx in corpus.facts.items():
        if fx["form"] in ("meets", "MACRO"):
            continue
        form = fx["form"] if fx["form"] in FORMS else "misplaced"
        rows[key] = (form, "")
    path = os.path.join(root, TSV)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write(render_tsv(rows, "baseline", lib.head_sha(root), version))
    meets = sum(1 for fx in corpus.facts.values() if fx["form"] == "meets")
    macro = sum(1 for fx in corpus.facts.values() if fx["form"] == "MACRO")
    dox = sum(1 for fx in corpus.facts.values() if fx["ranges"])
    say("doc-ratchet: baseline: %d entries, %d definitions, %d doxygen-form, %d "
        "meet, MACRO %d, %s -> %s" % (corpus.scored, corpus.definitions, dox,
                                      meets, macro, row_counts(rows), TSV))
    two = [k for k, fx in corpus.facts.items() if fx.get("two_headers")]
    dup = [k for k, fx in corpus.facts.items() if fx["form"] == "duplicate"]
    lib.print_bins({"TWO-HEADERS": two, "DUPLICATE": dup,
                    "NO-HEADER-INCLUDE": no_header_include(corpus)})
    return 0


def mode_armed(root):
    if os.environ.get("OPENPETE_NO_COMMITS") == "1":
        say("doc-ratchet: OPENPETE_NO_COMMITS=1; hook arming not required.")
        return 0
    why = lib.armed_reason(root, ("doc_ratchet.py",))
    if why:
        say("doc-ratchet: DISARMED: %s.\n  Arm this clone once:  %s"
            % (why, lib.ARM))
        return 1
    say("doc-ratchet: ARMED (%s)" % lib.HOOK_REL)
    return 0


def dispatch(root, mode, argv):
    if mode == "hook":
        return mode_hook(root)
    if mode == "check" and "--armed" in argv:
        return mode_armed(root)
    if mode == "check":
        return mode_check(root, argv)
    if mode == "tighten":
        return mode_tighten(root)
    if mode == "baseline":
        return mode_baseline(root)
    return None


if __name__ == "__main__":
    sys.exit(lib.run(FENCE, dispatch, __doc__))
