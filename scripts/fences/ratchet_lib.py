#!/usr/bin/env python3
"""Shared machinery of the per-function ratchet fences.

A ratchet fence keys every in-scope function as `<path>:<name>`, keeps a
baseline TSV of the functions that do not meet its requirement, and decides
in a git pre-commit hook over the index. This module holds what the fences
share; each fence supplies its scorer, its decision function, its touched
rule and its scope roots (`Fence`).

  - compile database lookup: exact entries per in-scope TU, entry flags;
  - the configuration of that database (OPENPETE_GAMES from the
    CMakeCache.txt beside it) and the UNCONFIGURED bin of a partial one;
  - the UNSCORED list and its self-check;
  - the staged changes (`git diff --cached --name-status -M`), the
    rename carry (identical blob or not), the acted-on paths and the
    MOVED / STALE-DB bins;
  - the after-side hunks of a diff, from which each fence builds its
    touched rule;
  - baseline TSV I/O (`# key: value` header lines with `written-by:` and
    `head:`, one column line, rows keyed by the first two columns) and
    the baseline source rule (a staged `tighten`/`baseline` file, else
    HEAD's; a staged hook-written file is stale by construction);
  - the hook's exit protocol and the arming check of the tracked hook;
  - the libclang TU parse and the definition walk, with the comment
    accessors the python bindings do not export (ctypes).
"""
import json
import os
import re
import shlex
import subprocess
import sys

DB_DEFAULT = "build/linux-release/compile_commands.json"
CONFIGURE = "cmake --preset linux-release"
HOOK_REL = "githooks/pre-commit"
ARM = "git config core.hooksPath githooks"
EMPTY_TREE = "4b825dc642cb6eb9a060e54bf8d69288fbee4904"

SCOPE_EXCLUDE = re.compile(
    r"(^|/)(generated|third_party|build)/"
    r"|^games/(spyro-[123])/\3/")
SOURCE_EXT = (".c", ".cc", ".cpp", ".cxx")
HEADER_EXT = (".h", ".hh", ".hpp", ".inc")

# Tracked in-scope sources that no linux-release target compiles. Acted on
# without refusal, never given rows. Self-checked on every run: each path is
# tracked and absent from the database, else the list is stale.
UNSCORED = (
    "games/spyro-1/dist/recomp_build.cpp",      # OPENPETE_RECOMP_LINKAGE=shared
    "games/spyro-1/recomp_attach_shared.c",     # OPENPETE_RECOMP_LINKAGE=shared
    "tests/recomp/gte_sideeffects/harness.c",   # built by check_recomp_gte.sh
    "tests/unit/sdk_windows_include_test.c",    # cross-compiled by check_sdk_windows_include.sh
    "core/mods/sdk/openpete_mod_abi.c",         # shipped SDK source, built with mods
)

# Tracked sources outside games/ that a target compiles only when the named
# game is configured. Under a partial OPENPETE_GAMES without that game they
# are out of configuration; with it, the completeness check requires their
# entry like any other source.
GAME_GATED = {
    # TARGET openpete-spyro1: compiles the mods against its staged SDK
    "tests/unit/mod_shader_compile_test.c": "spyro-1",
}

OWN_ROOT = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", ".."))


class Refusal(Exception):
    pass


class Configuration:
    """The OPENPETE_GAMES list of the build whose compile database a fence
    loads, against the list's default (`set(OPENPETE_GAMES ...)` in the
    top-level CMakeLists.txt), compared as sets.

    Under a partial list, a path under `games/<g>/` for an unconfigured
    `<g>`, or under `games/common/` (its sources compile only through a
    game's source list), or a GAME_GATED path whose game is unconfigured,
    is out of configuration: the database cannot
    describe it, so its tracked sources and baseline rows fall in the
    UNCONFIGURED bin, keyed by path."""

    def __init__(self, games, default, source):
        self.games = frozenset(games)
        self.default = frozenset(default)
        self.source = source

    @property
    def partial(self):
        return self.games != self.default

    def out_of_config(self, rel):
        if not self.partial:
            return False
        if rel in GAME_GATED:
            return GAME_GATED[rel] not in self.games
        m = re.match(r"games/([^/]+)/", rel)
        return bool(m) and m.group(1) not in self.games

    def describe(self):
        return "OPENPETE_GAMES=\"%s\" (%s)" % (
            ";".join(sorted(self.games)), self.source)


def default_games(root):
    """The default OPENPETE_GAMES list of the top-level CMakeLists.txt, or
    None for a tree that declares none (a fence test's scratch repository)."""
    try:
        with open(os.path.join(root, "CMakeLists.txt")) as fh:
            text = fh.read()
    except OSError:
        return None
    m = re.search(r'^set\(OPENPETE_GAMES\s+"([^"]*)"\s+CACHE', text, re.M)
    return [g for g in m.group(1).split(";") if g] if m else None


def read_configuration(root, db_path):
    """-> Configuration of the CMakeCache.txt beside db_path; a missing
    cache, a cache with no OPENPETE_GAMES entry, or a tree with no default
    list is the full default."""
    default = default_games(root)
    if default is None:
        return Configuration((), (), "no OPENPETE_GAMES default in the tree")
    cache = os.path.join(os.path.dirname(os.path.abspath(db_path)), "CMakeCache.txt")
    try:
        with open(cache) as fh:
            lines = fh.read().splitlines()
    except OSError:
        return Configuration(default, default, "no %s: full default" % cache)
    for line in lines:
        m = re.match(r"OPENPETE_GAMES(?::[A-Z]+)?=(.*)$", line)
        if m:
            games = [g for g in m.group(1).split(";") if g]
            return Configuration(games, default, cache)
    return Configuration(default, default, "%s has no OPENPETE_GAMES: full default" % cache)


class Fence:
    """The per-fence parameters.

    name          message prefix (`complexity-ratchet`)
    script        the fence's file name under scripts/fences/
    tsv           baseline path, repo-relative
    scope_roots   path prefixes a scope path starts with
    db_env        env var overriding the compile database path
    config        Configuration of the loaded database (load_db sets it;
                  the full default until then)
    """

    def __init__(self, name, script, tsv, scope_roots, db_env):
        self.name = name
        self.script = script
        self.tsv = tsv
        self.scope_roots = tuple(scope_roots)
        self.db_env = db_env
        self.config = Configuration((), (), "no database loaded")

    def say(self, msg=""):
        say(msg)

    def in_scope(self, rel):
        return (rel.startswith(self.scope_roots)
                and not SCOPE_EXCLUDE.search(rel)
                and rel.endswith(SOURCE_EXT + HEADER_EXT))

    def root_dirs(self):
        return [r.rstrip("/") for r in self.scope_roots]


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


def key_file(key):
    return key.rsplit(":", 1)[0]


def key_name(key):
    return key.rsplit(":", 1)[1]


# --------------------------------------------------------------------------
# Compile database
# --------------------------------------------------------------------------
def load_db(root, fence):
    """-> (path, {tu relpath: [entry]}) or (path, None) when absent. Sets
    fence.config from the CMakeCache.txt beside the database."""
    path = os.environ.get(fence.db_env) or os.path.join(root, DB_DEFAULT)
    if not os.path.isfile(path):
        return path, None
    fence.config = read_configuration(root, path)
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
        if rel.startswith("..") or not fence.in_scope(rel) or is_header(rel):
            continue
        entries.setdefault(rel, []).append(e)
    return path, entries


def entry_flags(entry):
    """The entry's compile tokens minus the compiler, -c, -o <obj> and the
    source file."""
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


def check_unscored(root, entries, fence, complete=False):
    """The UNSCORED list against the tree. complete (check, tighten,
    baseline): a tracked in-scope source with no entry and no UNSCORED line
    is refused too, unless it is out of configuration; the hook skips that
    half, since the new path of a staged move has no entry until the source
    list follows it. -> the out-of-configuration tracked sources with no
    entry (complete only), sorted."""
    if os.path.realpath(root) != os.path.realpath(OWN_ROOT):
        return []
    unscored = [p for p in UNSCORED if fence.in_scope(p)]
    unconfigured = []
    if complete:
        no_entry = sorted(
            f for f in git(root, "ls-files", "--", *fence.root_dirs()
                           ).stdout.split("\n")
            if f and fence.in_scope(f) and not is_header(f) and f not in entries
            and f not in UNSCORED and os.path.isfile(os.path.join(root, f)))
        unconfigured = [f for f in no_entry if fence.config.out_of_config(f)]
        missing = [f for f in no_entry if not fence.config.out_of_config(f)]
        if missing:
            raise Refusal(
                "tracked sources with no compile-database entry and no UNSCORED "
                "line: %s. Wire each into CMake and re-run `%s`, or add it to "
                "UNSCORED in scripts/fences/ratchet_lib.py with the "
                "reason no linux-release target compiles it."
                % (", ".join(missing), CONFIGURE))
    gated = [p for p in GAME_GATED if fence.in_scope(p)]
    tracked = set(git(root, "ls-files", "--", *(unscored + gated)).stdout.split("\n"))
    for p in gated:
        if p not in tracked:
            raise Refusal("GAME_GATED list is stale: %s is not tracked. Edit "
                          "GAME_GATED in scripts/fences/ratchet_lib.py." % p)
    for p in unscored:
        if p not in tracked or p in entries:
            raise Refusal("UNSCORED list is stale: %s is %s. Edit UNSCORED in "
                          "scripts/fences/ratchet_lib.py." % (
                              p, "in the compile database" if p in entries
                              else "not tracked"))
    say("%s: UNSCORED (no linux-release entry): %d sources"
        % (fence.name, len(unscored)))
    return unconfigured


def split_unconfigured(config, rows):
    """-> (rows in configuration, rows out of it), by the key's path. The
    second set is carried unchanged into a written baseline."""
    held = {k: v for k, v in rows.items() if config.out_of_config(key_file(k))}
    return {k: v for k, v in rows.items() if k not in held}, held


def report_unconfigured(fence, sources, rows):
    """Print the UNCONFIGURED bin's counts (nothing under the full default)."""
    if fence.config.partial:
        say("%s: UNCONFIGURED under %s: %d sources with no entry, %d rows held"
            % (fence.name, fence.config.describe(), len(sources), len(rows)))


def refuse_unconfigured(fence, paths):
    """The hook: a staged path out of configuration is refused, naming the
    configure that covers it (games/common/ needs the full default list)."""
    bad = sorted({p for p in paths if p and fence.config.out_of_config(p)})
    if not bad:
        return
    need = set(fence.config.games)
    for p in bad:
        need.add(GAME_GATED.get(p) or p.split("/")[1])
    if "common" in need:
        need = set(fence.config.default)
    raise Refusal(
        "staged paths %s lie outside this build's configuration, %s: no database "
        "entry describes an unconfigured game or games/common/. Configure "
        "the build directory of that database with -DOPENPETE_GAMES=\"%s\" "
        "and commit again." % (", ".join(bad), fence.config.describe(),
                               ";".join(sorted(need))))


def refuse_partial(fence, mode):
    """tighten and baseline rewrite every row; a partial configure would
    retire or drop the rows of every unconfigured path."""
    if fence.config.partial:
        raise Refusal("`%s` needs every game configured; this database is "
                      "configured with %s. Reconfigure with the full default "
                      "\"%s\" (`%s -U OPENPETE_GAMES`)." % (
                          mode, fence.config.describe(),
                          ";".join(sorted(fence.config.default)), CONFIGURE))


def present_scope_files(root, fence):
    out = set()
    for line in git(root, "ls-files", "--", *fence.root_dirs()).stdout.split("\n"):
        if line and fence.in_scope(line) and os.path.isfile(os.path.join(root, line)):
            out.add(line)
    return out


def wired(entries, present):
    """Paths with a compile-database entry; headers count as wired (their
    wiring is not readable: the database has no includer source)."""
    return set(entries) | {f for f in present if is_header(f)}


def dirty_paths(root, fence):
    out = set()
    p = git(root, "status", "--porcelain", "--", *fence.root_dirs())
    for line in p.stdout.splitlines():
        for part in line[3:].split(" -> "):
            out.add(part.strip('"'))
    return out


def head_sha(root):
    p = git(root, "rev-parse", "-q", "--verify", "HEAD", check=False)
    return p.stdout.strip() if p.returncode == 0 else ""


def head_tsv(root, tsv):
    p = git(root, "show", "HEAD:" + tsv, check=False)
    return p.stdout if p.returncode == 0 else None


# --------------------------------------------------------------------------
# The index
# --------------------------------------------------------------------------
def refuse_temporary_index():
    if "next-index" in os.environ.get("GIT_INDEX_FILE", ""):
        raise Refusal(
            "this commit form (`git commit <pathspec>`, `--only`, "
            "`--pathspec-from-file`) commits from a temporary index; the "
            "baseline the hook stages would land while the real index keeps "
            "the old blob. Use `git add <paths>` then `git commit`.")


def staged_changes(root):
    """-> ([(status letter, path, new path or None)], diff base)."""
    base = "HEAD" if head_sha(root) else EMPTY_TREE
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


def scope_changes(changes, in_scope):
    return [(s, a, b) for s, a, b in changes
            if in_scope(b or a) or (s in "DR" and in_scope(a))]


def acted_on_paths(root, base, scope):
    """The rename carry and the acted-on paths of the staged scope changes.
    -> (renames {old: (new, identical_blob)}, acted_on paths, need_entry
    paths (scored, so they need a database entry), moved old paths)."""
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
    return renames, acted_on, need_entry, moved


def carry_key(key, renames):
    f, name = key.rsplit(":", 1)
    if f in renames:
        return renames[f][0] + ":" + name
    return key


def diff_hunks(root, base, paths, cached=True):
    """-> {path: [(start, count)]}: each hunk's after-side start line and
    added-line count (0 = a pure deletion after line `start`). cached: the
    index against `base`; else the working tree against the index."""
    hunks = {}
    if not paths:
        return hunks
    args = ["diff", "-U0", "-M", "-l", "100000"]
    args += ["--cached", base] if cached else []
    p = git(root, *(args + ["--"] + list(paths)))
    cur = None
    for line in p.stdout.splitlines():
        if line.startswith("+++ "):
            cur = line[6:] if line.startswith("+++ b/") else None
        elif line.startswith("@@") and cur:
            m = re.match(r"@@ -\S+ \+(\d+)(?:,(\d+))? @@", line)
            hunks.setdefault(cur, []).append(
                (int(m.group(1)), int(m.group(2) if m.group(2) is not None else 1)))
    return hunks


def added_ranges(hunks):
    """{path: [(first, last)]} of added lines."""
    return {p: [(s, s + c - 1) for s, c in hs if c]
            for p, hs in hunks.items()}


def unstaged_refusal(root, acted_on, tsv):
    """Refuse an acted-on path whose working file differs from the index,
    and a working-copy baseline that differs from the staged one. -> the
    dirty path set."""
    dirty = set(git(root, "diff", "--name-only").stdout.split("\n"))
    unstaged = sorted(f for f in acted_on if f in dirty)
    if unstaged:
        raise Refusal("staged with unstaged hunks: %s. The commit carries the "
                      "index blob and the tool read the working file. "
                      "`git add` them, or stash the rest." % ", ".join(unstaged))
    if tsv in dirty:
        raise Refusal("%s differs between the working tree and the index "
                      "(tighten output not staged?). `git add %s`, or "
                      "`git checkout -- %s`." % (tsv, tsv, tsv))
    return dirty


# --------------------------------------------------------------------------
# Baseline TSV
# --------------------------------------------------------------------------
def parse_header(text):
    head = {}
    for line in text.splitlines():
        if not line.startswith("#"):
            break
        m = re.match(r"#\s*([\w-]+):\s*(.*)", line)
        if m:
            head[m.group(1)] = m.group(2).strip()
    return head


def parse_rows(text, ncols):
    """-> [columns] of every row with at least ncols columns (the column
    line excluded)."""
    out = []
    for line in text.splitlines():
        if line.startswith("#"):
            continue
        cols = line.split("\t")
        if len(cols) < ncols or cols[0] == "file":
            continue
        out.append(cols)
    return out


def render(title, head_lines, columns, rows, written_by, sha):
    """title: first comment line; head_lines: [(key, value)] before
    written-by/head; rows: [(key, [column values after file, function])]."""
    out = ["# " + title]
    out += ["# %s: %s" % kv for kv in head_lines]
    out += ["# written-by: " + written_by, "# head: " + sha,
            "\t".join(columns)]
    for key, cols in sorted(rows, key=lambda r: r[0]):
        f, name = key.rsplit(":", 1)
        out.append("\t".join([f, name] + list(cols)))
    return "\n".join(out) + "\n"


def staged_tsv(root, tsv, changes):
    """-> the staged baseline text, or None when the commit stages none."""
    if not any(tsv in (a, b) for _, a, b in changes):
        return None
    p = git(root, "show", ":" + tsv, check=False)
    return p.stdout if p.returncode == 0 else None


def reset_stale_hook_tsv(root, fence, staged_text):
    """A commit with no scope path: a staged hook-written baseline was left
    by a commit that failed after its hook passed; landing it here would
    revert rows tightened since. -> True when it was unstaged."""
    if staged_text is None or parse_header(staged_text).get("written-by") != "hook":
        return False
    git(root, "restore", "--staged", "--", fence.tsv)
    git(root, "checkout", "--", fence.tsv, check=False)
    say("%s: unstaged a stale hook-written %s" % (fence.name, fence.tsv))
    return True


def write_staged(root, tsv, text, staged_text):
    """Write the baseline and stage it when it differs from the working copy
    or from the staged copy."""
    path = os.path.join(root, tsv)
    current = open(path).read() if os.path.isfile(path) else None
    if text != current or staged_text not in (None, text):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as fh:
            fh.write(text)
        git(root, "add", "--", tsv)


# --------------------------------------------------------------------------
# Reports
# --------------------------------------------------------------------------
def print_bins(bins, quiet=(), limit=12):
    for name in sorted(bins):
        items = bins[name]
        if not items:
            continue
        say("  %-12s %d" % (name, len(items)))
        if name in quiet:
            continue
        for it in items[:limit]:
            if isinstance(it, tuple):
                key, b, a = it
                say("      %s  %s -> %s" % (key, "-" if b is None else b,
                                            "-" if a is None else a))
            else:
                say("      %s" % it)
        if len(items) > limit:
            say("      ... %d more" % (len(items) - limit))


def accept_reason(var):
    val = os.environ.get(var)
    if val is not None and val.strip() in ("", "0", "1"):
        raise Refusal("%s carries the reason; an empty value, 0 or 1 is "
                      "refused." % var)
    return val


# --------------------------------------------------------------------------
# Arming and entry point
# --------------------------------------------------------------------------
def hook_calls(text, script):
    return ('%s" hook' % script) in text or ("%s hook" % script) in text


def armed_reason(root, scripts):
    """-> None when the resolved pre-commit hook is the tracked
    githooks/pre-commit, executable, and calls `<script> hook` for every
    script; else the reason it is not."""
    p = git(root, "rev-parse", "--git-path", "hooks/pre-commit")
    resolved = os.path.realpath(os.path.normpath(
        os.path.join(root, p.stdout.strip())))
    want = os.path.realpath(os.path.join(root, HOOK_REL))
    if resolved != want:
        return "git resolves the pre-commit hook to %s" % resolved
    if not os.path.isfile(want):
        return "%s is missing" % HOOK_REL
    if not os.access(want, os.X_OK):
        return "%s is not executable (git skips it)" % HOOK_REL
    text = open(want).read()
    for script in scripts:
        if not hook_calls(text, script):
            return "%s does not call %s hook" % (HOOK_REL, script)
    return None


def repo_root(fence, mode):
    here = os.getcwd()
    p = subprocess.run(["git", "-C", here, "rev-parse", "--show-toplevel"],
                       capture_output=True, text=True)
    root = p.stdout.strip() if p.returncode == 0 else OWN_ROOT
    # A build directory's cwd can sit inside a nested checkout or outside any
    # repo; the fence's own repo is the default then.
    if mode != "hook" and not os.path.isfile(
            os.path.join(root, "scripts", "fences", fence.script)) \
            and not os.environ.get(fence.db_env):
        root = OWN_ROOT
    return root


def run(fence, dispatch, usage):
    """The exit protocol: dispatch(root, mode, argv) returns the exit code;
    a Refusal prints and exits 1 (git aborts the commit on nonzero)."""
    argv = sys.argv[1:]
    mode = argv[0] if argv else ""
    root = repo_root(fence, mode)
    try:
        code = dispatch(root, mode, argv)
        if code is None:
            say(usage)
            return 2
        return code
    except Refusal as e:
        say("%s: REFUSED: %s" % (fence.name, e))
        return 1


# --------------------------------------------------------------------------
# libclang: the TU parse, the definition walk, the comment accessors
# --------------------------------------------------------------------------
LIBCLANG = "/usr/lib/llvm-14/lib/libclang-14.so.1"
_CI = None


def libclang():
    """-> the configured clang.cindex module with the comment accessors
    bound. Refusal when the bindings or the library are missing."""
    global _CI
    if _CI is not None:
        return _CI
    import ctypes
    try:
        import clang.cindex as ci
    except ImportError:
        raise Refusal("python clang bindings missing. Install: pip install "
                      "--user 'clang==14.*' (and apt install libclang-14-dev)")
    if not ci.Config.loaded:
        path = os.environ.get("OPENPETE_LIBCLANG") or LIBCLANG
        if not os.path.isfile(path):
            raise Refusal("libclang not found at %s. Install: sudo apt install "
                          "libclang1-14" % path)
        ci.Config.set_library_file(path)
    lib = ci.conf.lib

    class CXComment(ctypes.Structure):
        _fields_ = [("ASTNode", ctypes.c_void_p), ("TU", ctypes.c_void_p)]

    def bind(name, args, res):
        fn = getattr(lib, name)
        fn.argtypes = args
        fn.restype = res

    bind("clang_Cursor_getParsedComment", [ci.Cursor], CXComment)
    bind("clang_Comment_getKind", [CXComment], ctypes.c_int)
    bind("clang_Comment_getNumChildren", [CXComment], ctypes.c_uint)
    bind("clang_Comment_getChild", [CXComment, ctypes.c_uint], CXComment)
    bind("clang_Comment_isWhitespace", [CXComment], ctypes.c_uint)
    bind("clang_TextComment_getText", [CXComment], ci._CXString)
    bind("clang_ParamCommandComment_getParamName", [CXComment], ci._CXString)
    bind("clang_ParamCommandComment_isParamIndexValid", [CXComment], ctypes.c_uint)
    bind("clang_BlockCommandComment_getCommandName", [CXComment], ci._CXString)
    bind("clang_BlockCommandComment_getParagraph", [CXComment], CXComment)
    bind("clang_InlineContentComment_hasTrailingNewline", [CXComment],
         ctypes.c_uint)
    bind("clang_InlineCommandComment_getNumArgs", [CXComment], ctypes.c_uint)
    bind("clang_InlineCommandComment_getArgText", [CXComment, ctypes.c_uint],
         ci._CXString)
    bind("clang_Cursor_getCommentRange", [ci.Cursor], ci.SourceRange)
    bind("clang_getClangVersion", [], ci._CXString)
    ci.CXComment = CXComment
    _CI = ci
    return ci


def cxstring(ci, s):
    return ci._CXString.from_result(s)


def libclang_version():
    ci = libclang()
    return cxstring(ci, ci.conf.lib.clang_getClangVersion())


def parse_tu(path, flags):
    """Parse a TU under its flags. -> clang TranslationUnit."""
    ci = libclang()
    return ci.Index.create().parse(path, flags)


def definition_cursors(tu):
    """Function cursors (declarations and definitions) over the TU cursor's
    children, descending into extern "C" blocks (UNEXPOSED_DECL and
    LINKAGE_SPEC), namespaces, classes, structs and class templates only."""
    ci = libclang()
    K = ci.CursorKind
    fn = {K.FUNCTION_DECL, K.CXX_METHOD, K.CONSTRUCTOR, K.DESTRUCTOR}
    desc = {K.UNEXPOSED_DECL, K.LINKAGE_SPEC, K.NAMESPACE, K.CLASS_DECL,
            K.STRUCT_DECL, K.CLASS_TEMPLATE}
    out, stack = [], [tu.cursor]
    while stack:
        cur = stack.pop()
        for ch in cur.get_children():
            if ch.kind in fn:
                out.append(ch)
            elif ch.kind in desc:
                stack.append(ch)
    return out
