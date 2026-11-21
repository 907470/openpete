#!/usr/bin/env python3
"""surface_docs.py - the surface baseline, the macro converter, the identity
check and the snippet check of the User docs.

Subcommands
-----------
  baseline --game G --dump FILE [--source-sha SHA]
      Write packaging/surface/<G>.json: the OPENPETE_SURFACE_DUMP output
      FILE with `source_sha` (default: HEAD of this checkout) added.

  convert [--check] [--require-conversion]
      Replace each marked literal on the User pages that equals one
      rendering of one surface macro for the marker's id (in the marker's
      game scope) with that macro, keeping the marker. The text before the
      marker on its line must end with the rendering, or with a `default()`
      rendering inside one code span; a marker that opens its line, a
      literal that is no rendering, a literal two macros render, and a key
      literal whose line already holds another element of the same binding
      stay map-only. Prints converted and unconverted counts per id kind,
      lists every unconverted marker, then runs the identity check on the
      result and writes the pages only when it passes. --check writes
      nothing and lists the convertible literals. --require-conversion
      exits 1 (VACUOUS) when nothing converts.

  identity [--before-rev REV] [--baseline-dir DIR] [--fresh-dump-dir DIR]
      For each User page, the expansion of the page under the baseline must
      equal, byte for byte, the pre-conversion page: the page at git REV
      when given (the converter's run), else the converter's output on the
      page's own expansion (the committed conversion is what the converter
      writes, and expands back to the literal). Then a negative control per
      macro kind mutates the field its rendering reads in a copy of the
      baseline and requires the expansion to change; a kind with no macro
      is reported VACUOUS and does not fail. --baseline-dir reads the
      baselines from DIR. --fresh-dump-dir DIR compares each baseline with
      DIR/<game>.json (a fresh OPENPETE_SURFACE_DUMP; source_sha ignored)
      and refuses on any difference.

  snippets --build-dir DIR
      Expand each User page, then run every TOML block through
      `<exe> --config FILE --config-report` and every command line (a code
      line or inline code span whose first word is an openpete executable
      or the AppImage) through `<exe> --no-config ARGS --config-report`,
      with the --disc and --ingest values and bare positional paths
      replaced by a fixture .cue path and --appimage-* runtime flags
      dropped. Each must exit 0. A page under games/<game>.md runs that
      game's executable; every other page runs each shipped game's. Exit
      64 when an executable is missing.

Exit codes: 0 pass, 1 refusal or VACUOUS, 2 usage or macro error, 64 skip.
"""

# sandbox-guard: intentional new user boot: --config-report runs in a scratch HOME and
# XDG roots with every OPENPETE_* variable removed, to read a snippet as a player's config.
import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts" / "lib"))
import surface_macros as sm  # noqa: E402
import game_conf  # noqa: E402

ROOT = sm.ROOT
USER_DIR = sm.USER_DIR
NAMESPACES = ("flag", "knob", "key", "menu", "directive")
BOUNDARY_BAD = re.compile(r"[A-Za-z0-9`+\-_.:/\\'\"]")


def user_pages():
    return sorted(USER_DIR.rglob("*.md"))


def rel(path):
    return path.relative_to(USER_DIR).as_posix()


# -- converter ---------------------------------------------------------


def marker_scope(words, page_g):
    """(id, game) of a marker's words: the @<game> suffix, else the page's."""
    game = page_g
    for w in words[1:]:
        if w.startswith("@"):
            game = w[1:]
    return words[0], game


def candidates(surface, ident, game):
    """Every (kind, n, rendering) of a macro for the id in the scope; a
    rendering that raises is not a candidate."""
    out = []

    def add(kind, n=1):
        g = None if kind in ("config_key", "flag") else game
        try:
            out.append((kind, n, surface.render(kind, ident, g, n)))
        except sm.MacroError:
            pass

    if ident.startswith(("knob:", "flag:")):
        for kind in ("default", "flag", "config_key"):
            add(kind)
    elif ident.startswith("key:"):
        for g in surface.games_for(game):
            e = surface.index[g].get(ident)
            if e is None:
                return out
            b = (surface.scoped_binding(ident, g) if game else None) or e["def"]
            nk = max(1, len(sm.keyboard_elems(b)))
            npad = len([t for t in b.split(",")
                        if t.strip().startswith("pad:")])
            break
        for n in range(1, nk + 1):
            add("key", n)
        for n in range(1, npad + 1):
            add("key_pad", n)
    return out


def literal_span(prefix, kind, r, floor):
    """(start, end) of the rendering r as the whole literal ending the
    stripped prefix, or None. end excludes a closing backtick."""
    if kind == "default" and prefix.endswith("`" + r + "`"):
        start = len(prefix) - len(r) - 1
        opening = start - 1
        if opening >= floor and (opening == 0 or prefix[opening - 1] != "`"):
            return start, start + len(r)
        return None
    if not prefix.endswith(r):
        return None
    start = len(prefix) - len(r)
    if start < floor:
        return None
    if start > 0 and BOUNDARY_BAD.match(prefix[start - 1]):
        return None
    if kind == "default" and prefix[floor:start].count("`") % 2:
        return None  # a bare rendering inside an open code span
    return start, start + len(r)


def other_elements(surface, ident, game, kind, n):
    """Renderings of the binding's elements other than (kind, n)."""
    out = []
    for k, m, r in candidates(surface, ident, game):
        if k in ("key", "key_pad") and (k, m) != (kind, n):
            out.append(r)
    return out


def ends_with_macro(text):
    """True when the text ends with a macro, bare or as one code span."""
    body = text[:-1] if text.endswith("`") else text
    last = None
    for last in sm.MACRO_RE.finditer(body):
        pass
    return last is not None and last.end() == len(body)


def convert_line(line, page_g, surface, sites, unconv, where):
    """The line with its convertible marked literals replaced by macros."""
    out, last = "", 0
    for m in sm.MARKER_RE.finditer(line):
        words = m.group(1).split()
        ident, game = marker_scope(words, page_g)
        ns = ident.split(":", 1)[0]
        prefix = line[last:m.start()]
        full = out + prefix
        stripped = full.rstrip()
        ws = full[len(stripped):]
        floor = len(out)
        reason = None
        if not line[:m.start()].strip():
            reason = "marker opens its line"
        elif ends_with_macro(stripped):
            reason = "already a macro"
        elif ns not in ("knob", "flag", "key"):
            reason = f"no macro renders a {ns}: id"
        elif game is not None and game not in surface.index:
            reason = f"no baseline for {game}"
        hits = []
        if reason is None:
            for kind, n, r in candidates(surface, ident, game):
                span = literal_span(stripped, kind, r, floor)
                if span:
                    hits.append((kind, n, r, span))
            if not hits:
                reason = "literal is no macro rendering"
            elif len(hits) > 1:
                reason = "literal is the rendering of " + ", ".join(
                    f"{k}({n})" for k, n, _r, _s in hits)
        if reason is None:
            kind, n, r, (s, e) = hits[0]
            if kind in ("key", "key_pad"):
                rest = stripped[:s]
                if any(o in rest for o in
                       other_elements(surface, ident, game, kind, n)):
                    reason = "line holds another element of the binding"
        if reason is None:
            macro = sm.macro_text(
                kind, ident,
                None if kind in ("config_key", "flag") else game,
                n if kind in ("key", "key_pad") else None)
            stripped = stripped[:s] + macro + stripped[e:]
            sites.append((where, ns, kind, ident, macro, r))
            out = stripped + ws + m.group(0)
        else:
            unconv.append((where, ns, ident, reason))
            out = full + m.group(0)
        last = m.end()
    return out + line[last:]


def convert_text(text, rel_path, surface):
    """(converted text, converted sites, unconverted markers)."""
    sites, unconv = [], []
    page_g = sm.page_game(rel_path)
    lines = []
    for i, line in enumerate(text.split("\n")):
        if "<!--" in line:
            line = convert_line(line, page_g, surface, sites, unconv,
                                f"{rel_path}:{i + 1}")
        lines.append(line)
    return "\n".join(lines), sites, unconv


def report_counts(sites, unconv, verb):
    for ns in NAMESPACES:
        c = sum(1 for s in sites if s[1] == ns)
        u = sum(1 for s in unconv if s[1] == ns)
        print(f"surface_docs: {ns:<9} {verb} {c:>3}  unconverted {u:>3}")
    print(f"surface_docs: total     {verb} {len(sites):>3}  unconverted "
          f"{len(unconv):>3}")


# -- identity check ----------------------------------------------------


MUTATORS = {
    "default": lambda e: e.__setitem__("def", (e.get("def") or "") + "0"),
    "key": lambda e: e.__setitem__("def", "F13," + e["def"]
                                   if e["def"] != "none" else "F13"),
    "key_pad": lambda e: e.__setitem__("def", "pad:guide," + e["def"]),
    "config_key": lambda e: e.__setitem__("config_key",
                                          e["config_key"] + "_x"),
    "flag": lambda e: e.__setitem__("flag", e["flag"] + "-x"),
}


def mutation_target(surface, kind, ident, game):
    """The baseline entries a kind's rendering reads for an id."""
    out = []
    for g in surface.games_for(game):
        if kind == "config_key":
            out.append(surface.knob_of(ident, g))
        else:
            out.append(surface.entry(ident, g))
    return out


def negative_control(surface, pages_expanded, macro_sites):
    """Per macro kind: mutate the first site's field, require a change.
    Returns (failures, report lines)."""
    fails, lines = 0, []
    for kind in sm.KINDS:
        site = next((s for s in macro_sites if s[2] == kind), None)
        if site is None:
            lines.append(f"control {kind:<10} VACUOUS: no macro of this "
                         f"kind")
            continue
        where, _ns, _k, ident, macro, _r = site
        page = where.split(":")[0]
        twin = surface.clone()
        game = sm.parse_macro(sm.MACRO_RE.search(macro))[2]
        for e in mutation_target(twin, kind, ident, game):
            MUTATORS[kind](e)
        src = (USER_DIR / page).read_text() if page not in pages_expanded \
            else pages_expanded[page][0]
        try:
            mutated = sm.expand(src, twin, page)
        except sm.MacroError as exc:
            mutated = f"<error {exc}>"
        if mutated == pages_expanded[page][1]:
            lines.append(f"control {kind:<10} FAIL: mutating {ident} left "
                         f"{page} unchanged")
            fails += 1
        else:
            lines.append(f"control {kind:<10} ok ({ident} on {page})")
    return fails, lines


def macro_sites_of(text, rel_path):
    """(where, ns, kind, id, macro, None) of each macro on a page."""
    out = []
    for i, line in enumerate(text.split("\n")):
        for m in sm.MACRO_RE.finditer(line):
            kind, ident, _g, _n = sm.parse_macro(m)
            out.append((f"{rel_path}:{i + 1}", ident.split(":", 1)[0], kind,
                        ident, m.group(0), None))
    return out


def git_show(rev, path):
    r = subprocess.run(["git", "-C", str(ROOT), "show",
                        f"{rev}:{path.relative_to(ROOT).as_posix()}"],
                       capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None


def drift(surface, fresh_dir):
    """Lines naming each id whose baseline entry differs from a fresh dump."""
    out = []
    for g, base in surface.baselines.items():
        path = Path(fresh_dir) / f"{g}.json"
        if not path.is_file():
            out.append(f"{g}: no fresh dump {path}")
            continue
        fresh = json.loads(path.read_text())
        a = {e["id"]: e for e in base["entries"]}
        b = {e["id"]: e for e in fresh["entries"]}
        for i in sorted(set(a) | set(b)):
            if a.get(i) != b.get(i):
                what = ("added" if i not in a else "removed" if i not in b
                        else "changed")
                out.append(f"{g}: {i} {what} since the baseline "
                           f"(source_sha {base.get('source_sha')})")
    return out


def identity(surface, before_rev=None, pages=None, quiet=False):
    """Run the identity check over the pages ({rel: (source, before)} or
    the tree). Returns the exit code."""
    if pages is None:
        pages = {}
        for p in user_pages():
            src = p.read_text()
            if before_rev:
                before = git_show(before_rev, p)
                if before is None:
                    before = src  # a page added after REV holds no macro
            else:
                before = None
            pages[rel(p)] = (src, before)
    fails, all_sites, expanded = [], [], {}
    for r, (src, before) in sorted(pages.items()):
        try:
            exp = sm.expand(src, surface, r)
        except sm.MacroError as exc:
            fails.append(str(exc))
            continue
        sites = macro_sites_of(src, r)
        all_sites += sites
        expanded[r] = (src, exp)
        if before is None:
            redo, redo_sites, _u = convert_text(exp, r, surface)
            want = {(s[0], s[4]) for s in redo_sites}
            for s in sites:
                if (s[0], s[4]) not in want:
                    fails.append(f"{s[0]}: {s[4]} is not what the converter "
                                 f"writes for the literal "
                                 f"'{sm.expand(s[4], surface, r)}'")
            if sm.expand(redo, surface, r) != exp:
                fails.append(f"{r}: the reconversion expands differently")
        elif exp != before:
            el, bl = exp.split("\n"), before.split("\n")
            for i in range(max(len(el), len(bl))):
                a = el[i] if i < len(el) else "<eof>"
                b = bl[i] if i < len(bl) else "<eof>"
                if a != b:
                    fails.append(f"{r}:{i + 1}: expands to\n    {a}\n  but "
                                 f"the pre-conversion page has\n    {b}")
    nctl, ctl_lines = negative_control(surface, expanded, all_sites)
    if not quiet:
        for line in ctl_lines:
            print(f"surface_docs: {line}")
    for f in fails:
        print(f"surface_docs: REFUSED: {f}")
    print(f"surface_docs: identity: {len(expanded)} pages, "
          f"{len(all_sites)} macros, {len(fails)} differences, "
          f"{nctl} control failures")
    if not all_sites:
        print("surface_docs: VACUOUS: no macro on any User page")
        return 1
    return 1 if fails or nctl else 0


# -- snippet check -----------------------------------------------------


EXE_RE = re.compile(r"^(?:\./)?(openpete-(spyro\d)(?:\.exe)?"
                    r"|OpenPete-\*\.AppImage)$")
FENCE_RE = re.compile(r"^(\s*)(```|~~~)\s*(\w*)")
INLINE_RE = re.compile(r"`([^`\n]+)`")
PATH_FLAGS = ("--disc", "--ingest")
DISC_EXT = (".cue", ".bin", ".img", ".iso", ".chd", ".pbp")


def snippets_of(text):
    """(line, kind, payload) of each TOML block and command line."""
    out, fence, buf = [], None, []
    for i, line in enumerate(text.split("\n")):
        m = FENCE_RE.match(line)
        if m:
            if fence is None:
                fence, buf = (m.group(3), i + 1), []
            else:
                if fence[0] == "toml":
                    out.append((fence[1], "toml", "\n".join(buf)))
                fence = None
            continue
        if fence is not None:
            buf.append(line)
            if fence[0] != "toml":
                words = line.split()
                if words and EXE_RE.match(words[0]):
                    out.append((i + 1, "cmd", line.strip()))
            continue
        for im in INLINE_RE.finditer(line):
            words = im.group(1).split()
            if words and EXE_RE.match(words[0]):
                out.append((i + 1, "cmd", im.group(1)))
    return out


def command_argv(cmd, exe, fixture):
    words = shlex.split(cmd)
    args, i = [], 1
    while i < len(words):
        w = words[i]
        if w.startswith("--appimage-"):
            i += 1
            continue
        if w in PATH_FLAGS and i + 1 < len(words):
            args += [w, fixture]
            i += 2
            continue
        if not w.startswith("-") and w.lower().endswith(DISC_EXT):
            args.append(fixture)  # a bare disc path means --ingest
        else:
            args.append(w)
        i += 1
    return list(exe) + ["--no-config"] + args + ["--config-report"]


def exe_game(cmd):
    """The game slug an executable word names; the AppImage is Spyro 1."""
    m = EXE_RE.match(shlex.split(cmd)[0])
    return f"spyro-{m.group(2)[-1]}" if m and m.group(2) else "spyro-1"


class SnippetRunner:
    """Runs snippets through `--config-report` in a scratch HOME with the
    OPENPETE_* environment removed."""

    CONTROL = 'fov = "not a number"'

    def __init__(self, build_dir, tmp, verbose):
        self.build = Path(build_dir).resolve()
        self.tmp = Path(tmp)
        self.fixture = self.tmp / "fixture.cue"
        self.fixture.write_text("")
        self.verbose = verbose
        self.env = {k: v for k, v in os.environ.items()
                    if not k.startswith("OPENPETE_")}
        self.env.update(HOME=str(self.tmp),
                        XDG_CONFIG_HOME=str(self.tmp / "cfg"),
                        XDG_DATA_HOME=str(self.tmp / "data"),
                        XDG_STATE_HOME=str(self.tmp / "state"))
        self.runs, self.fails, self.missing = 0, [], set()

    def exe(self, game):
        """The argv prefix running `game` from the build tree."""
        return game_conf.tree_argv(str(self.build), game)

    def argv(self, kind, payload, exe):
        if kind == "toml":
            cfg = self.tmp / "snippet.toml"
            cfg.write_text(payload + "\n")
            return list(exe) + ["--config", str(cfg), "--config-report"]
        return command_argv(payload, exe, str(self.fixture))

    def run(self, kind, payload, game):
        """Exit code of one snippet under one game's executable, or None
        when the executable is missing."""
        exe = self.exe(game)
        if not os.path.isfile(exe[0]):
            self.missing.add(exe[0])
            return None
        argv = self.argv(kind, payload, exe)
        res = subprocess.run(argv, capture_output=True, text=True,
                             env=self.env, cwd=self.tmp, timeout=60)
        if self.verbose:
            print(f"surface_docs: run {res.returncode} {kind} {game}: "
                  + " ".join(shlex.quote(a) for a in argv[1:]))
        self.last_err = res.stderr.strip().splitlines()
        return res.returncode

    def page(self, r, text, shipped):
        pg = sm.page_game(r)
        for line, kind, payload in snippets_of(text):
            if kind == "toml":
                games = [pg] if pg else list(shipped)
            else:
                games = [exe_game(payload)]
            for g in games:
                rc = self.run(kind, payload, g)
                if rc is None:
                    continue
                self.runs += 1
                if rc != 0:
                    err = self.last_err[0] if self.last_err else ""
                    self.fails.append(f"{r}:{line} ({kind}, {g}) exit "
                                      f"{rc}: {err}")


def snippets(surface, build_dir, verbose=False):
    with tempfile.TemporaryDirectory(prefix="surface-snippets-") as tmp:
        runner = SnippetRunner(build_dir, tmp, verbose)
        for p in user_pages():
            r = rel(p)
            try:
                text = sm.expand(p.read_text(), surface, r)
            except sm.MacroError as exc:
                runner.fails.append(str(exc))
                continue
            runner.page(r, text, surface.shipped)
        control = runner.run("toml", runner.CONTROL, surface.shipped[0])
    for f in runner.fails:
        print(f"surface_docs: REFUSED: {f}")
    if not runner.runs:
        print("surface_docs: SKIP: no executable: "
              + ", ".join(sorted(runner.missing)))
        return 64
    for m in sorted(runner.missing):
        print(f"surface_docs: not run (no executable): {m}")
    print(f"surface_docs: snippets: {runner.runs} runs, "
          f"{len(runner.fails)} refused")
    if control in (None, 0):
        print(f"surface_docs: VACUOUS: the control snippet "
              f"'{runner.CONTROL}' was not refused (exit {control})")
        return 1
    print(f"surface_docs: control '{runner.CONTROL}' refused (exit "
          f"{control})")
    return 1 if runner.fails else 0


# -- main --------------------------------------------------------------


def cmd_baseline(args):
    dump = json.loads(Path(args.dump).read_text())
    if dump.get("game") != args.game:
        print(f"surface_docs: {args.dump} is the {dump.get('game')} dump, "
              f"not {args.game}", file=sys.stderr)
        return 2
    sha = args.source_sha or subprocess.run(
        ["git", "-C", str(ROOT), "rev-parse", "HEAD"], capture_output=True,
        text=True, check=True).stdout.strip()
    out = {"source_sha": sha}
    out.update(dump)
    path = sm.SURFACE_DIR / f"{args.game}.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(out, indent=1) + "\n")
    print(f"surface_docs: wrote {path.relative_to(ROOT)} "
          f"({len(dump['entries'])} entries, source_sha {sha})")
    return 0


def cmd_convert(args, surface):
    pages, sites, unconv = {}, [], []
    for p in user_pages():
        src = p.read_text()
        new, s, u = convert_text(src, rel(p), surface)
        sites += s
        unconv += u
        pages[rel(p)] = (new, src)
    report_counts(sites, unconv, "convertible" if args.check else
                  "converted")
    for where, ns, ident, reason in unconv:
        if reason != "already a macro":
            print(f"surface_docs: map-only {where} {ident}: {reason}")
    if args.check:
        for where, _ns, _k, ident, macro, r in sites:
            print(f"surface_docs: convertible {where} {r!r} -> {macro}")
        return 0
    if args.require_conversion and not sites:
        print("surface_docs: VACUOUS: the converter converted nothing")
        return 1
    rc = identity(surface, pages=pages)
    added = sum(sm.count_macros(n) - sm.count_macros(o)
                for n, o in pages.values())
    print(f"surface_docs: macros added {added}, converted {len(sites)}")
    if added != len(sites):
        print("surface_docs: REFUSED: macros added != converted count")
        rc = 1
    if rc:
        print("surface_docs: identity check failed; pages not written")
        return rc
    for r, (new, src) in pages.items():
        if new != src:
            (USER_DIR / r).write_text(new)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--baseline-dir", default=str(sm.SURFACE_DIR))
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("baseline")
    b.add_argument("--game", required=True)
    b.add_argument("--dump", required=True)
    b.add_argument("--source-sha")
    c = sub.add_parser("convert")
    c.add_argument("--check", action="store_true")
    c.add_argument("--require-conversion", action="store_true")
    i = sub.add_parser("identity")
    i.add_argument("--before-rev")
    i.add_argument("--fresh-dump-dir")
    s = sub.add_parser("snippets")
    s.add_argument("--build-dir", required=True)
    s.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    if args.cmd == "baseline":
        return cmd_baseline(args)
    try:
        surface = sm.Surface(surface_dir=args.baseline_dir)
    except (OSError, KeyError, ValueError) as exc:
        print(f"surface_docs: cannot load the surface: {exc}",
              file=sys.stderr)
        return 2
    if args.cmd == "convert":
        return cmd_convert(args, surface)
    if args.cmd == "identity":
        rc = identity(surface, before_rev=args.before_rev)
        if args.fresh_dump_dir:
            d = drift(surface, args.fresh_dump_dir)
            for line in d:
                print(f"surface_docs: REFUSED: drift: {line}")
            print(f"surface_docs: drift: {len(d)} differences from the "
                  f"fresh dump")
            rc = rc or (1 if d else 0)
        return rc
    return snippets(surface, args.build_dir, args.verbose)


if __name__ == "__main__":
    sys.exit(main())
