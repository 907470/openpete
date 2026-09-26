#!/usr/bin/env python3
"""Fence: cognitive-complexity ratchet.

Every function over CAP cognitive complexity (clang-tidy 14,
readability-function-cognitive-complexity, IgnoreMacros) carries a row in
baselines/complexity/cognitive.tsv. A function's excess is
max(score, CAP) - CAP. A commit passes when, over the keys of the files it
stages, the sum of excess after is at most the sum before: a row never
rises unfunded, a split passes, a new function over the cap is funded only
by a reduction in the same commit.

Modes:
  hook              git pre-commit (githooks/pre-commit). Scores the working
                    tree, decides over the index, writes and stages the
                    baseline on PASS. Exit 1 aborts the commit.
  check             every build. The same inequality over TUs clean against
                    HEAD, against HEAD's baseline. Exit 1 on a rise or an
                    UNWIRED row; 64 = SKIP (no compile database) under --ctest.
  check --armed     exit 1 unless the resolved pre-commit hook is the tracked
                    githooks/pre-commit, executable, calling this fence, and
                    the baselines' merge driver (ratchet_tsv_merge.py) is
                    configured.
  tighten [--accept <key> <reason>]...
                    full-corpus rewrite at arc end: tightenings, retirements,
                    admissions; a rise is refused unless named by --accept.
  baseline          recapture the whole file (tool-version change only).

Escapes:
  // complexity-ratchet: <reason>        on the definition line or the line
                                         above: the function leaves the
                                         balance; its score is still recorded.
  OPENPETE_COMPLEXITY_ACCEPT="<reason>"  one-time accepted rise; the reason
                                         lands in the row's `accepted` column.
Environment:
  OPENPETE_COMPLEXITY_DB   compile database path (default
                           build/linux-release/compile_commands.json).
  OPENPETE_NO_COMMITS=1    a machine that builds and never commits passes
                           `check --armed` unarmed.
"""
import concurrent.futures
import json
import os
import re
import shlex
import shutil
import subprocess
import sys

import ratchet_tsv_merge as tsv_merge

CAP = 25
TOOL_MAJOR = 14
CHECK = "readability-function-cognitive-complexity"
CHECKS_ARG = "-checks=-*," + CHECK
CONFIG_ARG = ("-config={CheckOptions: ["
              "{key: %s.Threshold, value: 0}, "
              "{key: %s.IgnoreMacros, value: true}, "
              "{key: %s.DescribeBasicIncrements, value: true}]}"
              % (CHECK, CHECK, CHECK))
OPTIONS_LINE = "Threshold=0 IgnoreMacros=true header-filter=.* cap=%d" % CAP
JOBS = 16

TSV = "baselines/complexity/cognitive.tsv"
DB_DEFAULT = "build/linux-release/compile_commands.json"
CONFIGURE = "cmake --preset linux-release"
HOOK_REL = "githooks/pre-commit"
ARM = "git config core.hooksPath githooks"
MARKER = re.compile(r"//\s*complexity-ratchet:\s*(\S.*)")

SCOPE_ROOTS = ("core/", "games/", "tests/")
SCOPE_EXCLUDE = re.compile(
    r"(^|/)(generated|third_party|build|mods-attic)/"
    r"|^games/(spyro-[123])/\3/")
SOURCE_EXT = (".c", ".cc", ".cpp", ".cxx")
HEADER_EXT = (".h", ".hh", ".hpp", ".inc")

# Tracked in-scope sources that no linux-release target compiles. Acted on
# without refusal, never given rows. Self-checked on every run: each path is
# tracked and absent from the database, else the list is stale.
UNSCORED = (
    "games/spyro-1/dist/recomp_build.cpp",      # OPENPETE_RECOMP_LINKAGE=shared
    "games/spyro-1/recomp_attach_shared.c",     # OPENPETE_RECOMP_LINKAGE=shared
    "core/psx/pcsxr_gte_stub.c",                # MinGW only
    "tests/recomp/gte_sideeffects/harness.c",   # built by check_recomp_gte.sh
    "tests/unit/sdk_windows_include_test.c",    # cross-compiled by check_sdk_windows_include.sh
    "core/mods/sdk/openpete_mod_abi.c",         # shipped SDK source, built with mods
    # openpete_add_game() defaults that every linux-release game overrides
    "games/common/feature_list_none.c",
    "games/common/game_icon_none.c",
    "games/common/renderer/interp_regions_none.c",
    "games/common/renderer/layout_none.c",
    "games/common/renderer/moby_game_none.c",
    "games/common/renderer/orchestrator_none.c",
    "games/common/renderer/player_game_none.c",
    "games/common/renderer/terrain_game_none.c",
)

OWN_ROOT = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", ".."))
FIXTURE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "tests", "complexity_fixture")


class Refusal(Exception):
    pass


def say(msg=""):
    sys.stderr.write(msg + "\n")


def git(root, *args, check=True, env=None):
    p = subprocess.run(["git", "-C", root] + list(args),
                       capture_output=True, text=True, env=env)
    if check and p.returncode != 0:
        raise Refusal("git %s failed: %s" % (" ".join(args), p.stderr.strip()))
    return p


def is_header(path):
    return path.endswith(HEADER_EXT)


def in_scope(rel):
    return (rel.startswith(SCOPE_ROOTS) and not SCOPE_EXCLUDE.search(rel)
            and rel.endswith(SOURCE_EXT + HEADER_EXT))


def key_file(key):
    return key.rsplit(":", 1)[0]


def excess(score):
    return max(score, CAP) - CAP


# --------------------------------------------------------------------------
# The decision. Pure: every rule is a function of these inputs.
#
#   before_rows    {key: (score, accepted_reason)}   the baseline source
#   after_scores   {key: score}                      the scored working tree
#   acted_on       set of repo paths (new names, deleted names); None = all
#   renames        {old_path: (new_path, identical_blob)}
#   extents        {key: (first_line, last_line)}    for keys under suspicion
#   changed_ranges {path: [(first, last), ...]}      the commit's touched lines
#   markers        {key: reason}
#   wired_files    paths with a compile-database entry (headers: all present)
#   present_files  paths on disk
#   accept_reason  str (every refused rise) | {key: reason} (named rises) | None
#   unwired_all    refuse an UNWIRED row outside the acted-on set too
#
# Returns (verdict, new_rows, bins); verdict is "PASS" or "REFUSED"; bins maps
# a bin name to a list of (key, before_score, after_score).
# --------------------------------------------------------------------------
def decide(before_rows, after_scores, acted_on, renames, extents,
           changed_ranges, markers, wired_files, present_files,
           accept_reason, unwired_all=False):
    rows = {}
    identical_targets = set()
    for old, (new, identical) in renames.items():
        if identical:
            identical_targets.add(new)
    for key, val in before_rows.items():
        f, name = key.rsplit(":", 1)
        if f in renames:
            key = renames[f][0] + ":" + name
        rows[key] = val
    after = dict(after_scores)
    # An identical rename is not scored: the score is a function of content
    # and flags, both unchanged, and the new path has no database entry yet.
    for key, (score, _) in rows.items():
        if key_file(key) in identical_targets:
            after[key] = score

    bins = {b: [] for b in ("RISE", "ACCEPTED", "INHERITED", "MARKED",
                            "TIGHTENED", "RETIRED", "ADMITTED", "UNWIRED")}

    def acted(f):
        return acted_on is None or f in acted_on

    unwired = set()
    for key, (score, _) in rows.items():
        f = key_file(key)
        if (f in present_files and f not in wired_files
                and f not in identical_targets and not is_header(f)
                and (unwired_all or acted(f))):
            unwired.add(key)
            bins["UNWIRED"].append((key, score, None))

    def touched(key):
        if key not in extents:
            return True
        lo, hi = extents[key]
        return any(a <= hi and b >= lo
                   for a, b in changed_ranges.get(key_file(key), ()))

    keys = sorted(k for k in set(rows) | set(after)
                  if acted(key_file(k)) and k not in unwired)
    sum_before = sum_after = 0
    risers, inherited = [], set()
    for key in keys:
        if key in markers:
            continue
        row = rows[key][0] if key in rows else None
        score = after.get(key, 0)
        if score > (row if row is not None else CAP) and not touched(key):
            inherited.add(key)
            bins["INHERITED"].append((key, row, score))
            continue
        b, a = (row - CAP if row is not None else 0), excess(score)
        sum_before += b
        sum_after += a
        if a > b:
            risers.append((key, row, score, a - b))

    accepted = {}
    refused = []
    if sum_after > sum_before:
        refused = risers
        if accept_reason:
            remaining = []
            for r in risers:
                reason = (accept_reason if isinstance(accept_reason, str)
                          else accept_reason.get(r[0]))
                if reason:
                    accepted[r[0]] = reason
                    sum_after -= r[3]
                else:
                    remaining.append(r)
            refused = remaining if sum_after > sum_before else []
    for key, row, score, _ in risers:
        if key in accepted:
            bins["ACCEPTED"].append((key, row, score))
    refused_keys = {r[0] for r in refused}
    for key, row, score, _ in refused:
        bins["RISE"].append((key, row, score))

    new_rows = dict(rows)
    for key in keys:
        if key in inherited or key in refused_keys:
            continue
        row = rows.get(key)
        score = after.get(key, 0)
        if key in markers:
            bins["MARKED"].append((key, row[0] if row else None, score))
        if score > CAP:
            if key in accepted:
                reason = accepted[key]
            elif row is not None and score <= row[0]:
                reason = row[1]
            else:
                reason = ""
            new_rows[key] = (score, reason)
            if key not in markers and key not in accepted:
                if row is None:
                    bins["ADMITTED"].append((key, None, score))
                elif score < row[0]:
                    bins["TIGHTENED"].append((key, row[0], score))
        elif row is not None:
            del new_rows[key]
            bins["RETIRED"].append((key, row[0], score))

    verdict = "REFUSED" if (refused or unwired) else "PASS"
    return verdict, new_rows, bins


# --------------------------------------------------------------------------
# Baseline file
# --------------------------------------------------------------------------
def parse_tsv(text):
    """-> (header dict, {key: (score, accepted)})"""
    head, rows = {}, {}
    for line in text.splitlines():
        if line.startswith("#"):
            m = re.match(r"#\s*([\w-]+):\s*(.*)", line)
            if m:
                head[m.group(1)] = m.group(2).strip()
            continue
        cols = line.split("\t")
        if len(cols) < 5 or cols[0] == "file":
            continue
        rows[cols[0] + ":" + cols[1]] = (int(cols[2]), cols[4])
    return head, rows


def marked_keys(text):
    return {c[0] + ":" + c[1] for c in
            (line.split("\t") for line in text.splitlines())
            if len(c) >= 5 and c[3] == "1"}


def render_tsv(rows, markers, written_by, head_sha):
    out = ["# cognitive-complexity ratchet baseline (scripts/fences/complexity_ratchet.py)",
           "# tool: clang-tidy %d" % TOOL_MAJOR,
           "# options: " + OPTIONS_LINE,
           "# written-by: " + written_by,
           "# head: " + head_sha,
           "file\tfunction\tscore\tmarked\taccepted"]
    for key in sorted(rows):
        f, name = key.rsplit(":", 1)
        score, reason = rows[key]
        out.append("%s\t%s\t%d\t%s\t%s" % (
            f, name, score, "1" if key in markers else "0", reason))
    return "\n".join(out) + "\n"


# --------------------------------------------------------------------------
# Scoring
# --------------------------------------------------------------------------
def find_tool():
    for name in ("clang-tidy-%d" % TOOL_MAJOR, "clang-tidy"):
        path = shutil.which(name)
        if not path:
            continue
        out = subprocess.run([path, "--version"], capture_output=True,
                             text=True).stdout
        m = re.search(r"version (\d+)\.", out)
        if m and int(m.group(1)) == TOOL_MAJOR:
            return path
        found = m.group(1) if m else "unknown"
        if name == "clang-tidy":
            raise Refusal(
                "clang-tidy major version %s, the baseline is pinned to %d. "
                "Install clang-tidy-%d, or recapture under the new version: "
                "set TOOL_MAJOR and run `complexity_ratchet.py baseline`."
                % (found, TOOL_MAJOR, TOOL_MAJOR))
    raise Refusal("clang-tidy-%d not found. Install: sudo apt install "
                  "clang-tidy-%d clang-%d" % ((TOOL_MAJOR,) * 3))


def verify_config(tool):
    out = subprocess.run([tool, CHECKS_ARG, CONFIG_ARG, "--dump-config"],
                         capture_output=True, text=True, cwd="/").stdout
    ok = (re.search(r"^Checks:\s*'?(clang-diagnostic-\*,clang-analyzer-\*,)?-\*,%s'?\s*$"
                    % re.escape(CHECK), out, re.M)
          and re.search(r"IgnoreMacros\s*\n?\s*value:\s*'?true", out)
          and re.search(r"Threshold\s*\n?\s*value:\s*'?0'?\s*$", out, re.M))
    if not ok:
        raise Refusal("clang-tidy --dump-config does not show the pinned "
                      "check list and options; refusing to score.")


def load_db(root):
    path = os.environ.get("OPENPETE_COMPLEXITY_DB") or os.path.join(root, DB_DEFAULT)
    if not os.path.isfile(path):
        return path, None
    try:
        with open(path) as fh:
            db = json.load(fh)
    except ValueError:
        raise Refusal("compile database %s does not parse (a configure may be "
                      "rewriting it). Re-run: %s" % (path, CONFIGURE))
    entries = {}
    for e in db:
        f = os.path.normpath(os.path.join(e["directory"], e["file"]))
        rel = os.path.relpath(f, root)
        if rel.startswith("..") or not in_scope(rel) or is_header(rel):
            continue
        entries.setdefault(rel, []).append(e)
    return path, entries


def entry_flags(entry):
    toks = entry.get("arguments") or shlex.split(entry["command"])
    src = os.path.normpath(os.path.join(entry["directory"], entry["file"]))
    out, skip = [], False
    for t in toks[1:]:
        if skip:
            skip = False
        elif t == "-o":
            skip = True
        elif t == "-c":
            pass
        elif (not t.startswith("-")
              and os.path.normpath(os.path.join(entry["directory"], t)) == src):
            pass
        else:
            out.append(t)
    return out


DIAG = re.compile(r"^(.+?):(\d+):\d+: (warning|error|note): (.*)$")
FUNC = re.compile(r"function '(.+?)' has cognitive complexity of (\d+)")
INCR = re.compile(r"\+(\d+)(?:, including nesting penalty of \d+, "
                  r"nesting level increased to (\d+))?")


def run_tidy(tool, src, flags, cwd, root):
    """-> (functions [(relfile, line, name, score, increments)], errors)"""
    p = subprocess.run(
        [tool, CHECKS_ARG, CONFIG_ARG, "-header-filter=.*", "--quiet", src,
         "--"] + flags, capture_output=True, text=True, cwd=cwd)
    funcs, errors, cur = [], [], None
    for line in p.stdout.splitlines():
        m = DIAG.match(line)
        if not m:
            continue
        path, lineno, kind, msg = m.group(1), int(m.group(2)), m.group(3), m.group(4)
        if kind == "error":
            errors.append(line)
            cur = None
            continue
        rel = os.path.relpath(os.path.normpath(os.path.join(cwd, path)), root)
        if kind == "warning":
            fm = FUNC.search(msg)
            cur = None
            if fm and CHECK in msg and fm.group(1) != "operator()":
                cur = (rel, lineno, fm.group(1), int(fm.group(2)), [])
                funcs.append(cur)
        elif cur is not None:
            im = INCR.match(msg)
            if im:
                cur[4].append((lineno, int(im.group(1)), int(im.group(2) or 0)))
    if p.returncode != 0 and not errors and not funcs:
        errors.append((p.stderr.strip().splitlines() or ["clang-tidy exit %d"
                                                         % p.returncode])[-1])
    return funcs, errors


class Corpus:
    def __init__(self):
        self.scores = {}      # key -> score
        self.where = {}       # key -> (relfile, line, name)
        self.incr = {}        # key -> [(line, amount, nesting)]
        self.tu_of = {}       # key -> (tu relpath, entry)
        self.errors = {}      # tu relpath -> [error line]
        self.scored = 0       # TUs run
        self.diagnostics = 0  # cognitive-complexity diagnostics seen


def score_tus(tool, root, jobs, scope=in_scope):
    """jobs: [(tu_rel, src_abs, flags, cwd, entry)] -> Corpus. Functions are
    keyed per (file, line, name) at the maximum score across runs, then a
    repeated name within one file takes an ordinal by line order."""
    corpus = Corpus()
    sites = {}
    with concurrent.futures.ThreadPoolExecutor(JOBS) as pool:
        futs = {pool.submit(run_tidy, tool, j[1], j[2], j[3], root): j
                for j in jobs}
        for fut in concurrent.futures.as_completed(futs):
            tu_rel, _, _, _, entry = futs[fut]
            funcs, errors = fut.result()
            corpus.scored += 1
            if errors:
                corpus.errors.setdefault(tu_rel, []).extend(errors)
            for rel, line, name, score, incr in funcs:
                corpus.diagnostics += 1
                if rel.startswith("..") or not scope(rel):
                    continue
                site = (rel, line, name)
                if site not in sites or score > sites[site][0]:
                    sites[site] = (score, incr, tu_rel, entry)
    by_name = {}
    for (rel, line, name) in sites:
        by_name.setdefault((rel, name), []).append(line)
    for (rel, name), lines in by_name.items():
        for n, line in enumerate(sorted(lines)):
            key = "%s:%s" % (rel, name if n == 0 else "%s#%d" % (name, n + 1))
            score, incr, tu_rel, entry = sites[(rel, line, name)]
            corpus.scores[key] = score
            corpus.where[key] = (rel, line, name)
            corpus.incr[key] = incr
            corpus.tu_of[key] = (tu_rel, entry)
    return corpus


def find_markers(root, corpus):
    markers, cache = {}, {}
    for key, (rel, line, _) in corpus.where.items():
        if rel not in cache:
            try:
                with open(os.path.join(root, rel), errors="replace") as fh:
                    cache[rel] = fh.read().split("\n")
            except OSError:
                cache[rel] = []
        src = cache[rel]
        for n in (line - 1, line - 2):
            if 0 <= n < len(src):
                m = MARKER.search(src[n])
                if m:
                    markers[key] = m.group(1).strip()
                    break
    return markers


def function_extent(root, corpus, key):
    """(first, last) line of the function's definition, from clang's AST."""
    clang = shutil.which("clang-%d" % TOOL_MAJOR)
    if not clang or key not in corpus.where:
        return None
    rel, line, name = corpus.where[key]
    tu_rel, entry = corpus.tu_of[key]
    flags, cwd = (entry_flags(entry), entry["directory"]) if entry else (["-std=c11"], root)
    p = subprocess.run(
        [clang, "-fsyntax-only", "-Xclang", "-ast-dump=json", "-Xclang",
         "-ast-dump-filter=" + name, os.path.join(root, tu_rel)] + flags,
        capture_output=True, text=True, cwd=cwd)
    dec, text, pos = json.JSONDecoder(), p.stdout, 0
    while True:
        while pos < len(text) and text[pos] in " \r\n\t":
            pos += 1
        if pos >= len(text):
            return None
        try:
            node, pos = dec.raw_decode(text, pos)
        except ValueError:
            return None
        if (node.get("kind") in ("FunctionDecl", "CXXMethodDecl")
                and node.get("name") == name
                and any(c.get("kind") == "CompoundStmt"
                        for c in node.get("inner", ()))):
            loc = node.get("loc", {})
            loc = loc.get("expansionLoc", loc)
            if loc.get("line") not in (None, line):
                continue
            rng = node.get("range", {})
            begin = rng.get("begin", {}).get("line", line)
            end = rng.get("end", {}).get("line")
            if end is None:
                end = begin
            return (min(begin, line), max(end, line))


# --------------------------------------------------------------------------
# Negative controls: five fixtures, each with its own TU and rows, evaluated
# before every hook and check verdict.
# --------------------------------------------------------------------------
def run_controls(tool):
    with open(os.path.join(FIXTURE_DIR, "controls.json")) as fh:
        controls = json.load(fh)
    jobs = []
    for c in controls:
        for tu in c["tus"]:
            jobs.append((tu, os.path.join(FIXTURE_DIR, tu), ["-std=c11"],
                         FIXTURE_DIR, None))
    corpus = score_tus(tool, FIXTURE_DIR, jobs, scope=lambda rel: True)
    if corpus.errors or not corpus.scores:
        raise Refusal("negative controls: fixture TUs did not score clean: %s"
                      % (corpus.errors or "no diagnostics"))
    markers = find_markers(FIXTURE_DIR, corpus)
    failures = []
    for c in controls:
        after = {k: v for k, v in corpus.scores.items() if key_file(k) in c["tus"]}
        for k, want in c["scores"].items():
            if after.get(k) != want:
                failures.append("%s: %s scored %s, fixture expects %d"
                                % (c["name"], k, after.get(k), want))
        rows = {k: (v, "") for k, v in c["rows"].items()}
        verdict, _, bins = decide(
            rows, after, set(c["tus"]) | set(c.get("present", [])), {}, {}, {},
            {k: v for k, v in markers.items() if k in after},
            set(c["tus"]), set(c["tus"]) | set(c.get("present", [])), None)
        if verdict != c["verdict"] or (c.get("bin") and not bins[c["bin"]]):
            failures.append("%s: verdict %s, expected %s%s" % (
                c["name"], verdict, c["verdict"],
                " with bin " + c["bin"] if c.get("bin") else ""))
    if failures:
        raise Refusal("negative controls FAILED, no verdict is trusted:\n  "
                      + "\n  ".join(failures))
    return len(controls)


# --------------------------------------------------------------------------
# Shared corpus run
# --------------------------------------------------------------------------
def score_corpus(tool, root, entries, moved):
    """Score every in-scope database entry whose file exists. -> (corpus,
    bins) with MOVED (old path of a staged rename) and STALE-DB (absent
    file) entries skipped."""
    jobs, bins = [], {"MOVED": [], "STALE-DB": []}
    for rel, es in sorted(entries.items()):
        if not os.path.isfile(os.path.join(root, rel)):
            bins["MOVED" if rel in moved else "STALE-DB"].append(rel)
            continue
        for e in es:
            jobs.append((rel, os.path.join(root, rel), entry_flags(e),
                         e["directory"], e))
    corpus = score_tus(tool, root, jobs)
    if corpus.scored == 0 or corpus.diagnostics == 0:
        raise Refusal("VACUOUS: %d TUs scored, %d cognitive-complexity "
                      "diagnostics. No verdict." % (corpus.scored,
                                                    corpus.diagnostics))
    return corpus, bins


def check_unscored(root, entries, complete=False):
    """The UNSCORED list against the tree. complete (check, tighten,
    baseline): a tracked in-scope source with no entry and no UNSCORED line
    is refused too; the hook skips that half, since the new path of a staged
    move has no entry until the source list follows it."""
    if os.path.realpath(root) != os.path.realpath(OWN_ROOT):
        return
    if complete:
        missing = sorted(
            f for f in git(root, "ls-files", "--",
                           *[r.rstrip("/") for r in SCOPE_ROOTS]).stdout.split("\n")
            if f and in_scope(f) and not is_header(f) and f not in entries
            and f not in UNSCORED and os.path.isfile(os.path.join(root, f)))
        if missing:
            raise Refusal(
                "tracked sources with no compile-database entry and no UNSCORED "
                "line: %s. Wire each into CMake and re-run `%s`, or add it to "
                "UNSCORED in scripts/fences/complexity_ratchet.py with the "
                "reason no linux-release target compiles it."
                % (", ".join(missing), CONFIGURE))
    tracked = set(git(root, "ls-files", "--", *UNSCORED).stdout.split("\n"))
    for p in UNSCORED:
        if p not in tracked or p in entries:
            raise Refusal("UNSCORED list is stale: %s is %s. Edit UNSCORED in "
                          "scripts/fences/complexity_ratchet.py." % (
                              p, "in the compile database" if p in entries
                              else "not tracked"))
    say("complexity-ratchet: UNSCORED (no linux-release entry): %d sources"
        % len(UNSCORED))


def present_scope_files(root):
    out = set()
    for line in git(root, "ls-files", "--", *[r.rstrip("/") for r in SCOPE_ROOTS]
                    ).stdout.split("\n"):
        if line and in_scope(line) and os.path.isfile(os.path.join(root, line)):
            out.add(line)
    return out


def wired(entries, present):
    return set(entries) | {f for f in present if is_header(f)}


def head_sha(root):
    p = git(root, "rev-parse", "-q", "--verify", "HEAD", check=False)
    return p.stdout.strip() if p.returncode == 0 else ""


def head_tsv(root):
    p = git(root, "show", "HEAD:" + TSV, check=False)
    return p.stdout if p.returncode == 0 else None


def print_bins(bins, quiet=()):
    for name in sorted(bins):
        items = bins[name]
        if not items:
            continue
        say("  %-12s %d" % (name, len(items)))
        if name in quiet:
            continue
        for it in items[:12]:
            if isinstance(it, tuple):
                key, b, a = it
                say("      %s  %s -> %s" % (key, "-" if b is None else b,
                                            "-" if a is None else a))
            else:
                say("      %s" % it)
        if len(items) > 12:
            say("      ... %d more" % (len(items) - 12))


REFUSAL_OPTIONS = """
How to land this commit. The refactor is the end-state design, never the
smallest change that clears the number. Read every caller and every sibling
of the code being moved; choose the abstraction that its usages, the
surrounding subsystem and future maintenance call for; build that whole.
Refused by review even when the fence passes: a helper cut at an arbitrary
line, a function named for its position (`_part2`, `_inner`), a parameter
struct that exists only to carry locals across the cut, a wrapper that hides
a branch without removing it, a second copy of logic that already exists.
When the right abstraction is larger than this commit, land it first as its
own no-behaviour-change commit, then land the feature on top of it.

  1. de-nest   the new block sits at depth; an early return, a guard
               clause, or a table lookup clears the rise when the nesting
               itself is the defect (nesting is what cognitive complexity
               charges).
  2. extract   move a block into a static function in this TU when the
               block is one operation with a name, a contract and inputs
               the callers would recognize. The commit passes while its
               total excess over %(cap)d does not rise; a new function over
               %(cap)d is funded by the reduction it came from.
  3. split     break the function along its phase boundaries into several
               functions, each owning one phase and its state; the same
               balance admits them, and each piece's nesting increments
               start at depth 0, so the pieces total below the original by
               the nesting charge.
  4. lift      if the new branch is `if (knob)`, decide the knob at the
               caller or in a dispatch table so this function sees one
               path.
  5. accept    OPENPETE_COMPLEXITY_ACCEPT="<reason>" git commit ...
               For a rise that is the end-state design. Never a deferral of
               the refactor: a deferred refactor is done now, in its own
               commit.
  6. exempt    `// complexity-ratchet: <reason>` on the definition line
               or the line above it. For table-shaped functions (flat
               flag chains); the score is still recorded, and removing
               the marker reinstates the ratchet at the then-current
               score.
  7. fence     if the score is a mis-count (macro, generated block), the
               fence is the defect: fix or retire it this session and say
               so in the commit.
""" % {"cap": CAP}


def print_refusal(root, corpus, bins, added):
    for key, row, score in bins["RISE"]:
        base = row if row is not None else CAP
        say("complexity-ratchet: REFUSED  %s  %s -> %d (+%d over %s)" % (
            key, row if row is not None else "no row", score, score - base,
            "its row" if row is not None else "the cap %d" % CAP))
        incr = corpus.incr.get(key, [])
        rel = key_file(key)
        ranges = added.get(rel, ())
        hits = [i for i in incr if any(a <= i[0] <= b for a, b in ranges)]
        label = "increments on changed lines (%d of %d):" % (len(hits), len(incr))
        if not hits:
            hits = sorted(incr, key=lambda i: (-i[2], -i[1], i[0]))[:5]
            label = ("no increment on a changed line; top increments by "
                     "nesting level (%d of %d):" % (len(hits), len(incr)))
        say("  " + label)
        try:
            with open(os.path.join(root, rel), errors="replace") as fh:
                src = fh.read().split("\n")
        except OSError:
            src = []
        for line, amount, nest in hits[:20]:
            text = src[line - 1].strip()[:60] if 0 < line <= len(src) else ""
            say("    %s:%d  +%d  nesting level %d, `%s`" % (
                os.path.basename(rel), line, amount, nest, text))
        if len(hits) > 20:
            say("    ... %d more" % (len(hits) - 20))
    if bins["UNWIRED"]:
        say("complexity-ratchet: REFUSED  UNWIRED rows (the file is on disk "
            "and no target compiles it):")
        for key, row, _ in bins["UNWIRED"]:
            say("    %s  row %d" % (key, row))
        say("  Wire the file into CMake and re-run `%s`, or delete it." % CONFIGURE)
    if bins["RISE"]:
        say(REFUSAL_OPTIONS)


# --------------------------------------------------------------------------
# hook
# --------------------------------------------------------------------------
def staged_changes(root):
    base = "HEAD" if head_sha(root) else "4b825dc642cb6eb9a060e54bf8d69288fbee4904"
    p = git(root, "diff", "--cached", "--name-status", "-M", "-l", "100000", base)
    if "rename detection was skipped" in p.stderr:
        raise Refusal("git skipped rename detection (%s); a move would read as "
                      "delete + add. Commit the move in smaller pieces."
                      % p.stderr.strip())
    out = []
    for line in p.stdout.splitlines():
        cols = line.split("\t")
        out.append((cols[0][0], cols[1], cols[2] if len(cols) > 2 else None))
    return out, base


def diff_ranges(root, base, paths):
    """-> (touched, added): {path: [(first, last)]} in new-file line numbers.
    touched includes the two lines around a pure deletion."""
    touched, added = {}, {}
    if not paths:
        return touched, added
    p = git(root, "diff", "--cached", "-U0", "-M", "-l", "100000", base, "--", *paths)
    cur = None
    for line in p.stdout.splitlines():
        if line.startswith("+++ "):
            cur = line[6:] if line.startswith("+++ b/") else None
        elif line.startswith("@@") and cur:
            m = re.match(r"@@ -\S+ \+(\d+)(?:,(\d+))? @@", line)
            start, count = int(m.group(1)), int(m.group(2) or 1)
            if count:
                added.setdefault(cur, []).append((start, start + count - 1))
                touched.setdefault(cur, []).append((start, start + count - 1))
            else:
                touched.setdefault(cur, []).append((start, start + 1))
    return touched, added


def validate_source(root, head, rows, corpus_scores):
    """The staged TSV, written by tighten or baseline, against HEAD."""
    sha = head_sha(root)
    htext = head_tsv(root)
    hhead, hrows = parse_tsv(htext) if htext is not None else ({}, {})
    by = head.get("written-by")
    if by == "baseline":
        if htext is not None and hhead.get("tool") == head.get("tool"):
            raise Refusal("staged %s is a `baseline` recapture, and HEAD already "
                          "carries a baseline under the same tool version. Use "
                          "`tighten`." % TSV)
        return
    if head.get("head") != sha:
        raise Refusal("staged %s was computed against %s, HEAD is %s. Re-run "
                      "`complexity_ratchet.py tighten`, stage it, commit."
                      % (TSV, head.get("head", "?")[:10], sha[:10]))
    for key, (score, reason) in rows.items():
        if reason:
            continue
        if key in hrows and score > hrows[key][0]:
            raise Refusal("staged %s raises %s (%d -> %d) with no `accepted` "
                          "reason: a hand edit. Use `tighten --accept`." % (
                              TSV, key, hrows[key][0], score))
        if key not in hrows and key in corpus_scores and score > corpus_scores[key]:
            raise Refusal("staged %s adds %s at %d, the function scores %d: a "
                          "hand edit." % (TSV, key, score, corpus_scores[key]))


def mode_hook(root):
    if "next-index" in os.environ.get("GIT_INDEX_FILE", ""):
        raise Refusal(
            "this commit form (`git commit <pathspec>`, `--only`, "
            "`--pathspec-from-file`) commits from a temporary index; the "
            "baseline the hook stages would land while the real index keeps "
            "the old blob. Use `git add <paths>` then `git commit`.")
    changes, base = staged_changes(root)
    tsv_staged = any(TSV in (a, b) for _, a, b in changes)
    scope = [(s, a, b) for s, a, b in changes
             if in_scope(b or a) or (s in "DR" and in_scope(a))]
    staged_text = None
    shead, srows = {}, {}
    if tsv_staged:
        p = git(root, "show", ":" + TSV, check=False)
        if p.returncode == 0:
            staged_text = p.stdout
            shead, srows = parse_tsv(staged_text)

    if not scope:
        if staged_text is not None and shead.get("written-by") == "hook":
            # Left by a commit that failed after its hook passed. Landing it
            # here would revert rows tightened since.
            git(root, "restore", "--staged", "--", TSV)
            git(root, "checkout", "--", TSV, check=False)
            say("complexity-ratchet: unstaged a stale hook-written %s" % TSV)
        elif staged_text is not None:
            validate_source(root, shead, srows, {})
        return 0

    tool = find_tool()
    verify_config(tool)
    db_path, entries = load_db(root)
    if entries is None:
        raise Refusal("no compile database at %s. Run: %s" % (db_path, CONFIGURE))
    controls = run_controls(tool)
    check_unscored(root, entries)

    renames, acted_on, need_entry, moved = {}, set(), [], set()
    for status, a, b in scope:
        if status == "R":
            same = (git(root, "rev-parse", ":" + b).stdout
                    == git(root, "rev-parse", base + ":" + a).stdout)
            renames[a] = (b, same)
            moved.add(a)
            acted_on.add(b)
            if not same:
                need_entry.append(b)
        elif status == "D":
            acted_on.add(a)
            moved.add(a)
        else:
            acted_on.add(a)
            need_entry.append(a)

    corpus, cbins = score_corpus(tool, root, entries, moved)

    for rel in need_entry:
        if is_header(rel) or rel in UNSCORED:
            continue
        if rel not in entries:
            raise Refusal("%s has no exact compile-database entry (clang-tidy "
                          "would borrow a neighbour's flags). Run: %s"
                          % (rel, CONFIGURE))
        if rel in corpus.errors:
            raise Refusal("%s does not parse clean; functions after an error "
                          "are dropped and would read as retirements:\n  %s"
                          % (rel, "\n  ".join(corpus.errors[rel][:5])))
    dirty = set(git(root, "diff", "--name-only").stdout.split("\n"))
    unstaged = sorted(f for f in acted_on if f in dirty)
    if unstaged:
        raise Refusal("staged with unstaged hunks: %s. The commit carries the "
                      "index blob and the tool read the working file. "
                      "`git add` them, or stash the rest." % ", ".join(unstaged))
    if TSV in dirty:
        raise Refusal("%s differs between the working tree and the index "
                      "(tighten output not staged?). `git add %s`, or "
                      "`git checkout -- %s`." % (TSV, TSV, TSV))
    dirty_headers = sorted(f for f in dirty if f and in_scope(f) and is_header(f))

    # Baseline source: a staged tighten/baseline file, else HEAD's; never the
    # working copy, never a staged hook-written file.
    if staged_text is not None and shead.get("written-by") in ("tighten", "baseline"):
        validate_source(root, shead, srows, corpus.scores)
        source_name, source_text, before = "staged (%s)" % shead["written-by"], staged_text, srows
    else:
        htext = head_tsv(root)
        if htext is None:
            raise Refusal("no baseline in HEAD or the index. Run "
                          "`complexity_ratchet.py baseline`, stage %s." % TSV)
        source_name, source_text, before = "HEAD", htext, parse_tsv(htext)[1]
        if staged_text is not None:
            source_name += " (staged hook-written file discarded)"

    # A header key reported through an includer that failed to parse is
    # unknown, not retired.
    after = dict(corpus.scores)
    if corpus.errors:
        for key, (score, _) in before.items():
            if is_header(key_file(key)) and key not in after:
                after[key] = score

    markers = find_markers(root, corpus)
    present = present_scope_files(root) | {f for f in acted_on
                                           if os.path.isfile(os.path.join(root, f))}
    touched, added = diff_ranges(root, base, sorted(
        f for f in acted_on if os.path.isfile(os.path.join(root, f))))
    accept = os.environ.get("OPENPETE_COMPLEXITY_ACCEPT")
    if accept is not None and accept.strip() in ("", "0", "1"):
        raise Refusal("OPENPETE_COMPLEXITY_ACCEPT carries the reason; an empty "
                      "value, 0 or 1 is refused.")

    args = (before, after, acted_on, renames)
    rest = (touched, markers, wired(entries, present), present)
    verdict, new_rows, bins = decide(*args, {}, *rest, accept)
    if bins["RISE"] or bins["ACCEPTED"]:
        extents = {}
        for key, _, _ in bins["RISE"] + bins["ACCEPTED"]:
            ext = function_extent(root, corpus, key)
            if ext:
                extents[key] = ext
        verdict, new_rows, bins = decide(*args, extents, *rest, accept)

    bins.update(cbins)
    bins["DIRTY-HEADER"] = dirty_headers
    say("complexity-ratchet: %d TUs scored, %d functions, %d controls; baseline "
        "source: %s; %d markers, %d accepted rows" % (
            corpus.scored, len(corpus.scores), controls, source_name,
            len(markers), sum(1 for v in new_rows.values() if v[1])))
    if bins["INHERITED"]:
        say("  INHERITED rises (not from this commit; repair: "
            "`complexity_ratchet.py tighten --accept <key> \"<reason>\"`):")
    print_bins({k: v for k, v in bins.items() if k not in ("RISE", "UNWIRED")},
               quiet=("MARKED",))
    if verdict == "REFUSED":
        print_refusal(root, corpus, bins, added)
        return 1
    if accept:
        for key, row, score in bins["ACCEPTED"]:
            say("complexity-ratchet: ACCEPTED  %s  %s -> %d  (%s)" % (
                key, row if row is not None else "no row", score, accept))

    if new_rows == before and marked_keys(source_text) == {
            k for k in new_rows if k in markers}:
        text = source_text
    else:
        text = render_tsv(new_rows, markers, "hook", head_sha(root))
    path = os.path.join(root, TSV)
    current = open(path).read() if os.path.isfile(path) else None
    if text != current or staged_text not in (None, text):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as fh:
            fh.write(text)
        git(root, "add", "--", TSV)
    say("complexity-ratchet: PASS")
    return 0


# --------------------------------------------------------------------------
# check / tighten / baseline
# --------------------------------------------------------------------------
def dirty_paths(root):
    out = set()
    p = git(root, "status", "--porcelain", "--", *[r.rstrip("/") for r in SCOPE_ROOTS])
    for line in p.stdout.splitlines():
        for part in line[3:].split(" -> "):
            out.add(part.strip('"'))
    return out


def corpus_inputs(root, ctest=False):
    db_path, entries = load_db(root)
    if entries is None:
        say("complexity-ratchet: SKIP, no compile database at %s" % db_path)
        sys.exit(64 if ctest else 0)
    tool = find_tool()
    verify_config(tool)
    controls = run_controls(tool)
    check_unscored(root, entries, complete=True)
    corpus, cbins = score_corpus(tool, root, entries, set())
    present = present_scope_files(root)
    dirty = dirty_paths(root)
    clean = {f for f in present | set(entries) if f not in dirty}
    return tool, entries, controls, corpus, cbins, present, dirty, clean


def clean_after(corpus, before, clean_tus):
    """Scores with the keys of error TUs' headers held at their rows."""
    after = dict(corpus.scores)
    if corpus.errors:
        for key, (score, _) in before.items():
            if is_header(key_file(key)) and key not in after:
                after[key] = score
    return after


def mode_check(root, ctest):
    tool, entries, controls, corpus, cbins, present, dirty, clean = \
        corpus_inputs(root, ctest)
    htext = head_tsv(root)
    if htext is None:
        raise Refusal("HEAD carries no %s. Run `complexity_ratchet.py "
                      "baseline` and commit it." % TSV)
    before = parse_tsv(htext)[1]
    clean_tus = [f for f in entries if f in clean
                 and os.path.isfile(os.path.join(root, f))]
    total_tus = [f for f in entries if os.path.isfile(os.path.join(root, f))]
    acted = {f for f in clean if f not in corpus.errors}
    # A row whose file left the tree is a retirement for tighten to write.
    acted |= {key_file(k) for k in before
              if not os.path.exists(os.path.join(root, key_file(k)))}
    markers = find_markers(root, corpus)
    verdict, _, bins = decide(before, clean_after(corpus, before, clean_tus),
                              acted, {}, {}, {}, markers,
                              wired(entries, present), present, None,
                              unwired_all=True)
    bins.update(cbins)
    bins["DIRTY"] = sorted(f for f in dirty if in_scope(f))
    bins["ERROR-TU"] = sorted(corpus.errors)
    say("complexity-ratchet: engaged %d/%d TUs clean against HEAD, %d functions, "
        "%d controls, %d rows, %d markers, %d accepted rows" % (
            len(clean_tus), len(total_tus), len(corpus.scores), controls,
            len(before), len(markers), sum(1 for v in before.values() if v[1])))
    if not clean_tus:
        raise Refusal("VACUOUS: no TU is clean against HEAD.")
    quiet = ("MARKED", "DIRTY") if verdict == "PASS" else ("MARKED",)
    print_bins({k: v for k, v in bins.items() if k not in ("RISE", "UNWIRED")},
               quiet=quiet)
    if any(bins[b] for b in ("TIGHTENED", "RETIRED", "ADMITTED")):
        say("  `complexity_ratchet.py tighten` writes the TIGHTENED, RETIRED "
            "and ADMITTED bins.")
    if verdict == "REFUSED":
        print_refusal(root, corpus, bins, {})
        say("A rise reached HEAD without the hook (merge, revert, "
            "--no-verify). Repair: reduce it, or `complexity_ratchet.py "
            "tighten --accept <key> \"<reason>\"` and commit the baseline.")
        return 1
    say("complexity-ratchet: PASS")
    return 0


def mode_tighten(root, argv):
    accept = {}
    i = 0
    while i < len(argv):
        if argv[i] == "--accept" and i + 2 < len(argv) + 0 and argv[i + 2].strip():
            accept[argv[i + 1]] = argv[i + 2].strip()
            i += 3
        else:
            raise Refusal("usage: tighten [--accept <key> \"<reason>\"]...")
    tool, entries, controls, corpus, cbins, present, dirty, clean = corpus_inputs(root)
    htext = head_tsv(root)
    if htext is None:
        raise Refusal("HEAD carries no %s. Run `baseline`." % TSV)
    before = parse_tsv(htext)[1]
    acted = {f for f in clean if f not in corpus.errors}
    acted |= {key_file(k) for k in before
              if not os.path.exists(os.path.join(root, key_file(k)))}
    markers = find_markers(root, corpus)
    verdict, new_rows, bins = decide(
        before, clean_after(corpus, before, None), acted, {}, {}, {}, markers,
        wired(entries, present), present, accept or None, unwired_all=True)
    unknown = sorted(k for k in accept if k not in {b[0] for b in bins["ACCEPTED"]})
    bins.update(cbins)
    bins["DIRTY"] = sorted(f for f in dirty if in_scope(f))
    print_bins({k: v for k, v in bins.items() if k not in ("RISE", "UNWIRED")},
               quiet=("MARKED",))
    for key in unknown:
        say("complexity-ratchet: --accept %s names no refused rise; ignored." % key)
    if new_rows == before and marked_keys(htext) == {
            k for k in new_rows if k in markers}:
        say("complexity-ratchet: %s is current (%d rows); nothing written."
            % (TSV, len(new_rows)))
    else:
        with open(os.path.join(root, TSV), "w") as fh:
            fh.write(render_tsv(new_rows, markers, "tighten", head_sha(root)))
        say("complexity-ratchet: wrote %s (%d rows). Stage and commit it." % (
            TSV, len(new_rows)))
    if verdict == "REFUSED":
        print_refusal(root, corpus, bins, {})
        say("Refused rises keep their rows. Accept one with "
            "`tighten --accept <key> \"<reason>\"`.")
        return 1
    return 0


def mode_baseline(root):
    tool, entries, controls, corpus, cbins, present, dirty, clean = corpus_inputs(root)
    bad = sorted(f for f in dirty if in_scope(f) and f != TSV)
    if bad:
        raise Refusal("baseline refuses dirty scored files (the rows would "
                      "describe uncommitted content): %s" % ", ".join(bad[:10]))
    if corpus.errors:
        raise Refusal("TUs with errors: %s\n  %s" % (
            ", ".join(sorted(corpus.errors)),
            "\n  ".join(e for es in corpus.errors.values() for e in es[:2])))
    markers = find_markers(root, corpus)
    rows = {k: (s, "") for k, s in corpus.scores.items() if s > CAP}
    os.makedirs(os.path.join(root, os.path.dirname(TSV)), exist_ok=True)
    with open(os.path.join(root, TSV), "w") as fh:
        fh.write(render_tsv(rows, markers, "baseline", head_sha(root)))
    scores = sorted(corpus.scores.values())
    say("complexity-ratchet: baseline: %d TUs, %d functions (median %d, p90 %d), "
        "%d rows over %d, %d marked -> %s" % (
            corpus.scored, len(scores), scores[len(scores) // 2],
            scores[len(scores) * 9 // 10], len(rows), CAP,
            sum(1 for k in rows if k in markers), TSV))
    print_bins(cbins)
    return 0


def mode_armed(root):
    if os.environ.get("OPENPETE_NO_COMMITS") == "1":
        say("complexity-ratchet: OPENPETE_NO_COMMITS=1, this machine declares "
            "it never commits; hook arming not required.")
        return 0
    p = git(root, "rev-parse", "--git-path", "hooks/pre-commit")
    resolved = os.path.realpath(os.path.normpath(
        os.path.join(root, p.stdout.strip())))
    want = os.path.realpath(os.path.join(root, HOOK_REL))
    why = None
    if resolved != want:
        why = "git resolves the pre-commit hook to %s" % resolved
    elif not os.path.isfile(want):
        why = "%s is missing" % HOOK_REL
    elif not os.access(want, os.X_OK):
        why = "%s is not executable (git skips it)" % HOOK_REL
    elif "complexity_ratchet.py\" hook" not in open(want).read() \
            and "complexity_ratchet.py hook" not in open(want).read():
        why = "%s does not call complexity_ratchet.py hook" % HOOK_REL
    arm = ARM
    if not why:
        p = git(root, "config", "--get", "merge.%s.driver" % tsv_merge.DRIVER_NAME,
                check=False)
        if p.stdout.strip() != tsv_merge.DRIVER_CONFIG:
            why = ("merge.%s.driver is %s; the baselines would merge as text "
                   "and conflict on their `# head:` line"
                   % (tsv_merge.DRIVER_NAME, repr(p.stdout.strip()) if
                      p.stdout.strip() else "unset"))
            arm = tsv_merge.ARM_MERGE
    if why:
        say("complexity-ratchet: DISARMED: %s.\n  Arm this clone once:  %s\n"
            "  A machine that builds and never commits sets "
            "OPENPETE_NO_COMMITS=1 in its build script." % (why, arm))
        return 1
    say("complexity-ratchet: ARMED (%s, merge driver %s)"
        % (HOOK_REL, tsv_merge.DRIVER_NAME))
    return 0


def main():
    argv = sys.argv[1:]
    mode = argv[0] if argv else ""
    here = os.getcwd()
    p = subprocess.run(["git", "-C", here, "rev-parse", "--show-toplevel"],
                       capture_output=True, text=True)
    root = p.stdout.strip() if p.returncode == 0 else OWN_ROOT
    # A build directory's cwd can sit inside a nested checkout or outside any
    # repo; the fence's own repo is the default then.
    if mode != "hook" and not os.path.isfile(
            os.path.join(root, "scripts", "fences", "complexity_ratchet.py")) \
            and not os.environ.get("OPENPETE_COMPLEXITY_DB"):
        root = OWN_ROOT
    try:
        if mode == "hook":
            return mode_hook(root)
        if mode == "check" and "--armed" in argv:
            return mode_armed(root)
        if mode == "check":
            return mode_check(root, "--ctest" in argv)
        if mode == "tighten":
            return mode_tighten(root, argv[1:])
        if mode == "baseline":
            return mode_baseline(root)
        say(__doc__)
        return 2
    except Refusal as e:
        say("complexity-ratchet: REFUSED: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
