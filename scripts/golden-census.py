#!/usr/bin/env python3
"""golden-census.py -- the tile census of the presentation golden.

A capture run with OPENPETE_TILE_CENSUS set writes `<tile base>.census`
beside each -vk PNG: `# gen <G> tree <T>`, then one `name<TAB>value` line per
nonzero counter of the scene the tile presents (core/diag/tile_census.def
names every counter and says what it counts).

The census store. scripts/golden-fleet.py keeps, per game unit, the
sidecars of its newest completed unfiltered full runs under
<local fleet_root>/census/<game>-<tier>/<run-id>/: the unit's `*-vk.census`,
tile-digests.tsv (each captured tile's digest16), moved.txt (the unit's
moved tiles from the stream), copies of the run's run-args.json and done
marker, and entry.json (run id, unit, head, source tree, unit rc, compared
and sidecar counts, time written), the last file written. A unit's counts
source is, among its entries with entry.json and done whose run-args.json
records none of filter, freecam_filter, rows and repeat, the newest whose
head is a first-parent ancestor of HEAD, else the newest, with a warning.
No reader falls back to an older entry, a `.part`, a filtered run or the
fleet's stream directories. Every answer opens with one line per unit read
naming its counts source. A manifest row of the checkout with no sidecar in
the entry (added since) is `unknown`: every `pred` of its game holds it.

Query files. One per commit, written by the change's author:

    # commit <sha>                            stamped by expect --query
    # counts <game> <run-id> <tree12>         stamped: each game's counts source
    reason <TAB> <one line: which faces the change decides differently>
    pred   <TAB> <game,game | all> <TAB> <predicate>
    hand   <TAB> <game> <TAB> <tile or row substring> <TAB> missing:<counter | none>

A file's hash is the SHA-256 of its lines that do not open with `#`, so
the stamp changes no hash. `fix FILE` appends the hash to fixed.tsv in the
file's directory on receipt; `expect --query` checks it, stamps the header,
writes `<sha>.tiles` (the `pred` selection in the counts source, `unknown`
rows included, and the `hand` lines) and appends that file's hash. A hand
list (`<sha>.hand`) is expected-moves lines, `<unit> <substring> <reason>`.

Subcommands:
  digests DIR                   write DIR/tile-digests.tsv: every -vk tile's digest16
  query (--game G|--all) PRED [--rows]
  expect (--game G|--all) PRED --reason TEXT
  expect --query FILE [--sha SHA] [--append FILE]
  fix FILE...                   append each file's hash to fixed.tsv beside it
  nearest --game G COUNTER [--top N]
  settles (--game G|--all)      the frozen-backdrop settle count and tally pass
                                time of the counts source's run (census-settle.tsv)
  score --run DIR [--window]    score a full run's query files (DIR/queries);
                                --window compares them with hand lists: a
                                pred line selecting 2 rows or fewer, or with
                                a launch: term or an upper bound (< <= ==),
                                which can name rows its author saw move,
                                scores as a hand line on both sides
  sidecar-diff DIR [DIR2]       sidecars byte for byte: two directories by
                                name, or one directory's repeat copies
Every reading subcommand takes --store DIR (default <local fleet_root>/census).

A predicate (PRED) is a conjunction joined by `and`: `counter OP integer`
(OP one of > >= < <= == !=), the leg terms canon, inter, ident, lodext,
orig, the scope terms own and stable, and `launch:<substring>` matched
against the row's manifest launch field. A counter term matches the tile's
own value or its `backdrop.` value unless the predicate says own. Under
stable an identity tile reads every leg-variable counter (TC_LEGVAR in the
registry: the moby and terrain counters and the pools that hold them,
which the sub-tick re-extract the identity leg composes can fill with
records the canonical scene lacks or drops, none of them drawn) from its
canon twin's sidecar. A counter added after
a sidecar was written (the sidecar's generation below the counter's `since`)
is unknown there, counts as engaged and prints `?`.

Exit: 0 done, 1 refused or no counts (printed), 2 usage.
"""
import argparse
import datetime
import fnmatch
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
REGISTRY = "core/diag/tile_census.def"
GAMES = ("spyro-1", "spyro-2", "spyro-3")
DIGESTS_NAME = "tile-digests.tsv"
MOVED_NAME = "moved.txt"
ENTRY_NAME = "entry.json"
FIXED_NAME = "fixed.tsv"
SETTLE_NAME = "census-settle.tsv"
DIGEST_HEX = 16
LEG_RE = re.compile(r"__(?:lodext__)?(canon|inter|ident)__(\d+)-vk\.png$")
# run-args.json keys that make a run filtered: such a run is never a counts source.
FILTER_KEYS = ("filter", "freecam_filter", "rows", "repeat")
MOVED = ("DIVERGE", "EXTRA", "EXCEED")
VERBS = ("SAME", "DIVERGE", "EXTRA", "IDENT", "EXCEED")
# A commit touches the renderer when it changes a file under one of these.
RENDERER_PATHS = ("core/renderer/*", "core/rhi/*", "core/mods/*", "games/common/renderer/*",
                  "games/spyro-*/renderer/*", "core/include/openpete_native_render.h",
                  "core/include/*.glsl", "core/runtime/run_capture.c",
                  "tests/harness/presentation-corpus.sh", "tests/harness/freecam-corpus.sh",
                  "scripts/presentation-game.sh", "scripts/corpus-resume.sh")


class Refused(Exception):
    """A refusal: printed, exit 1."""


def load_script(name, rel):
    """Import a script of the checkout as a module."""
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, rel))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


def load_pb():
    """scripts/presentation-blessed.py as a module."""
    return load_script("presentation_blessed", "scripts/presentation-blessed.py")


def git(*args, check=True, root=ROOT):
    """Run git in the checkout; return stdout, or None on failure when check is False."""
    p = subprocess.run(["git", "-C", root] + list(args), capture_output=True, text=True)
    if p.returncode != 0:
        if check:
            raise Refused("git %s: %s" % (" ".join(args), p.stderr.strip()))
        return None
    return p.stdout


def default_store():
    """<local fleet_root>/census, the local host's fleet_root read the way scripts/golden-fleet.py reads it."""
    try:
        gf = load_script("golden_fleet", "scripts/golden-fleet.py")
        return os.path.join(gf.local_fleet_root(), "census")
    except Exception:  # an unreadable host file: the default root
        return os.path.expanduser("~/.cache/openpete-fleet/census")


# ---- the registry ---------------------------------------------------------

def c_strings(text):
    """The C string literals of text, concatenated."""
    return "".join(bytes(m.group(1), "utf-8").decode("unicode_escape")
                   for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', text))


class Registry:
    """The rows of core/diag/tile_census.def."""

    def __init__(self, text):
        """Parse the generation, the path and family names, every TC_BIT and every TC_ROW."""
        body = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        m = re.search(r"TC_GEN\(\s*(\d+)\s*\)", body)
        if not m:
            raise Refused("%s: no TC_GEN" % REGISTRY)
        self.gen = int(m.group(1))
        self.paths = re.findall(r'TC_PATH\(\s*"([^"]+)"\s*\)', body)
        self.families = re.findall(r'TC_FAMILY\(\s*"([^"]+)"\s*\)', body)
        self.bits = {int(b): n for b, n in re.findall(r'TC_BIT\(\s*(\d+)\s*,\s*"([^"]+)"\s*\)', body)}
        self.legvar = [self.pattern(n) for n in re.findall(r'TC_LEGVAR\(\s*"([^"]+)"\s*\)', body)]
        self.rows = []
        for m in re.finditer(r"TC_ROW\((.*?)\)\s*(?=TC_ROW|TC_BIT|TC_LEGVAR|\Z)", body, flags=re.S):
            args = m.group(1)
            head = re.match(r'\s*(\w+)\s*,\s*"([^"]*)"\s*,\s*"(\w+)"\s*,\s*"(\w+)"\s*,'
                            r'\s*"([^"]*)"\s*,\s*(\d+)\s*,\s*(\w+)\s*,(.*)$', args, flags=re.S)
            if not head:
                raise Refused("%s: malformed TC_ROW(%s...)" % (REGISTRY, args[:40]))
            rid, name, unit, source, fam, since, state, rest = head.groups()
            self.rows.append({"id": rid, "name": name, "unit": unit, "source": source,
                              "family": fam, "since": int(since), "state": state,
                              "meaning": c_strings(rest), "re": self.pattern(name)})

    def pattern(self, name):
        """A compiled regex for a row's name pattern."""
        alt = lambda xs: "(?:%s)" % "|".join(re.escape(x) for x in xs)  # noqa: E731
        out, pos = "", 0
        for m in re.finditer(r"<(path|family|n)>", name):
            out += re.escape(name[pos:m.start()])
            out += {"path": alt(self.paths), "family": alt(self.families), "n": r"[0-9]+"}[m.group(1)]
            pos = m.end()
        return re.compile("^" + out + re.escape(name[pos:]) + "$")

    def leg_variable(self, name):
        """Whether a counter can differ between an identity tile and its canon twin of one image (TC_LEGVAR); backdrop counters cannot."""
        return not name.startswith("backdrop.") and any(r.match(name) for r in self.legvar)

    def live(self, name):
        """The live row a predicate or nearest may name; raises Refused for an unregistered or retired counter."""
        r = self.resolve(name)
        if r is None:
            raise Refused("%s is no registered counter (%s)" % (name, REGISTRY))
        if r["state"] == "retired":
            raise Refused("%s is retired: no sidecar writes it (%s)" % (name, REGISTRY))
        return r

    def resolve(self, name):
        """The row a counter name belongs to, or None. `backdrop.` prefixes a tally, bit or cache row."""
        for r in self.rows:
            if r["re"].match(name):
                return r
        if name.startswith("backdrop."):
            r = self.resolve(name[len("backdrop."):])
            if r and r["source"] in ("tally", "bit", "cache"):
                return r
        return None


def load_registry(text=None):
    """The registry of the working tree, or of the given text."""
    if text is None:
        with open(os.path.join(ROOT, REGISTRY), encoding="utf-8") as f:
            text = f.read()
    return Registry(text)


# ---- sidecars, digests, manifests -------------------------------------------

def read_sidecar(path):
    """(gen, tree, {name: value}) of a sidecar; raises Refused when unreadable."""
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError as e:
        raise Refused("sidecar %s: %s" % (path, e.strerror))
    m = re.match(r"^# gen (\d+) tree (\S+)$", lines[0] if lines else "")
    if not m:
        raise Refused("sidecar %s: first line is not `# gen G tree T`" % path)
    vals = {}
    for ln in lines[1:]:
        name, sep, v = ln.partition("\t")
        if not sep or not v.isdigit():
            raise Refused("sidecar %s: malformed line %r" % (path, ln))
        vals[name] = int(v)
    return int(m.group(1)), m.group(2), vals


def read_digests(d):
    """{tile: digest16} of a directory's tile-digests.tsv; raises when absent."""
    path = os.path.join(d, DIGESTS_NAME)
    out = {}
    try:
        with open(path, encoding="utf-8") as f:
            for ln in f:
                if ln.startswith("#") or not ln.strip():
                    continue
                out[ln.split("\t")[0]] = ln.rstrip("\n").split("\t")[1]
    except OSError as e:
        raise Refused("%s: %s" % (path, e.strerror))
    return out


def leg_of(tile):
    """canon, inter or ident."""
    m = LEG_RE.search(tile)
    return m.group(1) if m else ""


def pose_slug(level, pose):
    """A freecam row's tile key (scripts/presentation-game.sh pg_pose_slug)."""
    return "%s-%s" % (level.replace(":", "-"), hashlib.md5(pose.encode()).hexdigest()[:6])


def manifest_rows(game, tier="presentation"):
    """{row key: launch field} of a game tier's corpus manifests (bk2 rows and freecam poses)."""
    files = (["dragon-rescue-corpus.manifest"] if tier == "dragons"
             else ["presentation-corpus.manifest", "freecam-corpus.manifest"])
    rows = {}
    for f in files:
        try:
            with open(os.path.join(ROOT, "games", game, "corpus", f), encoding="utf-8") as fh:
                text = fh.read()
        except OSError:
            continue
        for ln in text.splitlines():
            if not ln.strip() or ln.lstrip().startswith("#") or ln.startswith("rows=") or "|" not in ln:
                continue
            fld = ln.split("|")
            name = fld[0].strip()
            if not name or name.startswith("#"):
                continue
            if f == "freecam-corpus.manifest":
                pose = fld[1].strip() if len(fld) > 1 else ""
                if pose:
                    rows[pose_slug(name, pose)] = fld[3].strip() if len(fld) > 3 else ""
            else:
                rows[name] = fld[5].strip() if len(fld) > 5 else ""
    return rows


def tile_row(tile):
    """A tile's row key: the name before its first `__`."""
    return tile.split("__", 1)[0]


def body_hash(path):
    """SHA-256 of a file's lines that do not open with `#`: the hash a stamp leaves unchanged."""
    h = hashlib.sha256()
    with open(path, encoding="utf-8") as f:
        for ln in f:
            if not ln.startswith("#"):
                h.update(ln.encode("utf-8"))
    return h.hexdigest()


def file_hash(path):
    """SHA-256 of a file's bytes."""
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def utc_now():
    """The current UTC time, ISO form to the second."""
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def touches_renderer(paths):
    """Whether a commit's changed paths include one on the renderer path list."""
    return any(fnmatch.fnmatch(p, pat) for p in paths for pat in RENDERER_PATHS)


# ---- the census store ----------------------------------------------------------

def read_json(path):
    """A JSON file's object, or None when absent or unreadable."""
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def entry_refusal(d):
    """Why a store directory is not a counts source, or None."""
    if d.endswith(".part"):
        return "unfinished (.part)"
    if read_json(os.path.join(d, ENTRY_NAME)) is None:
        return "no %s" % ENTRY_NAME
    if not os.path.exists(os.path.join(d, "done")):
        return "no done marker"
    ra = read_json(os.path.join(d, "run-args.json"))
    if ra is None:
        return "no run-args.json"
    keys = [k for k in FILTER_KEYS if ra.get(k)]
    return "filtered run (%s)" % ", ".join(keys) if keys else None


def unit_entries(store, unit):
    """[(run id, dir)] of a unit's qualifying entries, newest first (run ids sort by start time)."""
    d = os.path.join(store, unit)
    if not os.path.isdir(d):
        return []
    out = [(n, os.path.join(d, n)) for n in os.listdir(d) if os.path.isdir(os.path.join(d, n))]
    return sorted(((n, p) for n, p in out if entry_refusal(p) is None), reverse=True)


def first_parent_commits():
    """[sha] of HEAD's first-parent history, HEAD first."""
    return (git("rev-list", "--first-parent", "HEAD", check=False) or "").split()


def renderer_commits(commits):
    """How many of the commits touch the renderer (first-parent diffs)."""
    n = 0
    for c in commits:
        out = git("diff-tree", "--no-commit-id", "--name-only", "-r", "-m", "--first-parent", c,
                  check=False) or ""
        n += touches_renderer(out.split())
    return n


class Source:
    """One unit's counts source: the chosen store entry, its sidecars and the checkout's rows against it."""

    def __init__(self, unit, d, header):
        """Read the entry's sidecars, digests and moved tiles; mark the checkout's rows it lacks unknown."""
        self.unit, self.dir, self.header = unit, d, header
        self.game = unit.rsplit("-", 1)[0]
        self.entry = read_json(os.path.join(d, ENTRY_NAME))
        self.run_id = self.entry.get("run_id", os.path.basename(d))
        self.digests = read_digests(d)
        self.tiles = {}
        for t in self.digests:
            p = os.path.join(d, t[:-len(".png")] + ".census")
            if os.path.isfile(p):
                gen, _tree, vals = read_sidecar(p)
                self.tiles[t] = (gen, vals)
        try:
            with open(os.path.join(d, MOVED_NAME), encoding="utf-8") as f:
                self.moved = {ln.strip() for ln in f if ln.strip()}
        except OSError:
            self.moved = set()
        self.rows = {tile_row(t) for t in self.tiles}
        self.launch = manifest_rows(self.game)
        self.unknown = sorted(r for r in self.launch if r not in self.rows)
        self.removed = len(self.rows - set(self.launch))


def counts_source(store, unit, fp=None):
    """(Source or None, header line) of a unit by the first-parent ancestry rule."""
    entries = unit_entries(store, unit)
    if not entries:
        return None, ("[golden-census] %s: no completed unfiltered full run in %s; no counts"
                      % (unit, store))
    fp = first_parent_commits() if fp is None else fp
    pos = {c: i for i, c in enumerate(fp)}
    chosen, dist = None, None
    for rid, d in entries:
        e = read_json(os.path.join(d, ENTRY_NAME))
        if e.get("head") in pos:
            chosen, dist = (rid, d, e), pos[e["head"]]
            break
    rid, d, e = chosen or (entries[0][0], entries[0][1], read_json(os.path.join(entries[0][1], ENTRY_NAME)))
    done = datetime.datetime.fromtimestamp(os.path.getmtime(os.path.join(d, "done")),
                                           datetime.timezone.utc).strftime("%Y-%m-%dT%H:%MZ")
    head = "%s: run %s, head %s, tree %s, done %s" % (unit, rid, (e.get("head") or "?")[:12],
                                                     (e.get("source_tree") or "?")[:12], done)
    if chosen:
        line = "%s; HEAD is %d first-parent commits past it, %d touch the renderer" % (
            head, dist, renderer_commits(fp[:dist]))
    else:
        line = "%s; head %s is not an ancestor of HEAD; newest entry used" % (head, (e.get("head") or "?")[:12])
    src = Source(unit, d, "[golden-census] " + line)
    src.header += "; %d row(s) unknown (added since), %d removed since" % (len(src.unknown), src.removed)
    return src, src.header


def open_sources(store, games, quiet=False):
    """{game: Source} of the games with a counts source; prints every header and the unknown rows."""
    out = {}
    fp = first_parent_commits()
    for g in games:
        src, line = counts_source(store, "%s-presentation" % g, fp)
        if not quiet:
            print(line)
            if src and src.unknown:
                print("[golden-census] %s: unknown rows: %s" % (src.unit, " ".join(src.unknown)))
        if src:
            out[g] = src
    return out


# ---- predicates --------------------------------------------------------------

TERM_RE = re.compile(r"^([A-Za-z0-9_.]+)\s*(>=|<=|==|!=|>|<)\s*(\d+)$")
LEGS = ("canon", "inter", "ident", "lodext", "orig")
OPS = {">": lambda x, y: x > y, ">=": lambda x, y: x >= y, "<": lambda x, y: x < y,
       "<=": lambda x, y: x <= y, "==": lambda x, y: x == y, "!=": lambda x, y: x != y}
UPPER = ("<", "<=", "==")
SCOPES = ("own", "stable")


def parse_pred(text, registry=None):
    """[(kind, a, op, b)] terms of a predicate; a counter term must name a registered counter."""
    terms = []
    for raw in re.split(r"\s+and\s+", text.strip()):
        s = raw.strip()
        if not s:
            continue
        if s in LEGS or s in SCOPES:
            terms.append(("leg" if s in LEGS else s, s, None, None))
        elif s.startswith("launch:"):
            terms.append(("launch", s[len("launch:"):], None, None))
        else:
            m = TERM_RE.match(s.replace(" ", ""))
            if not m:
                raise Refused("predicate term %r is not `counter OP integer`, a leg term, own or launch:" % s)
            if registry is not None:
                try:
                    registry.live(m.group(1))
                except Refused as e:
                    raise Refused("predicate term %r: %s" % (s, e))
            terms.append(("ctr", m.group(1), m.group(2), int(m.group(3))))
    if not terms:
        raise Refused("empty predicate")
    return terms


def leg_holds(term, tile):
    """Whether a leg term holds for a tile."""
    if term == "lodext":
        return "__lodext__" in tile
    if term == "orig":
        return "__lodext__" not in tile
    return leg_of(tile) == term


def twin_of(tile):
    """The canon tile of an identity tile's capture (same row, LOD and VBL), or None for any other leg."""
    return tile.replace("__ident__", "__canon__") if leg_of(tile) == "ident" else None


def eval_tile(terms, tile, gen, vals, launch, registry, twin=None):
    """(holds, value, unknown) of a predicate on one tile; value is the first counter term's.

    Under `stable` an identity tile reads each leg-variable counter
    (Registry.leg_variable) from @p twin, its canon twin's (gen, vals),
    so the predicate holds on it exactly when it holds on the canonical
    scene of the same image; with no twin in the entry it reads its own.
    """
    own = any(k == "own" for k, *_ in terms)
    stable = twin is not None and any(k == "stable" for k, *_ in terms)
    value, unknown, first = 0, False, True
    for kind, a, op, b in terms:
        if kind == "leg" and not leg_holds(a, tile):
            return False, 0, False
        if kind == "launch" and a not in launch.get(tile_row(tile), ""):
            return False, 0, False
        if kind != "ctr":
            continue
        g, vv = twin if stable and registry.leg_variable(a) else (gen, vals)
        row = registry.resolve(a)
        if row and g < row["since"]:
            unknown = True
            continue
        v, vb = vv.get(a, 0), vv.get("backdrop." + a, 0)
        if first:
            value, first = max(v, 0 if own else vb), False
        if not (OPS[op](v, b) or (not own and OPS[op](vb, b))):
            return False, 0, False
    return True, value, unknown


def select(sources, terms, registry):
    """([(game, tile, value, unknown)], [(game, row)]): the tiles a predicate holds on, and the unknown rows it holds."""
    hits, unk = [], []
    for g, src in sorted(sources.items()):
        for tile, (gen, vals) in src.tiles.items():
            tw = twin_of(tile)
            ok, v, u = eval_tile(terms, tile, gen, vals, src.launch, registry,
                                 src.tiles.get(tw) if tw else None)
            if ok:
                hits.append((g, tile, v, u))
        unk += [(g, r) for r in src.unknown]
    return hits, unk


def games_of(a):
    """The games a query names."""
    return list(GAMES) if a.all else [a.game]


def cmd_query(a):
    """Print the tiles (or rows) a predicate selects, by value; exit 1 when no unit has a counts source."""
    registry = load_registry()
    terms = parse_pred(a.pred, registry)
    sources = open_sources(a.store, games_of(a))
    if not sources:
        return 1
    hits, unk = select(sources, terms, registry)
    if a.rows:
        agg = {}
        for g, tile, v, u in hits:
            k = (g, tile_row(tile))
            m, n, uu = agg.get(k, (0, 0, False))
            agg[k] = (max(m, v), n + 1, uu or u)
        for (g, r), (m, n, u) in sorted(agg.items(), key=lambda kv: (-kv[1][0], kv[0])):
            print("%s\t%s\tmax=%d%s\ttiles=%d" % (g, r, m, "?" if u else "", n))
        for g, r in unk:
            print("%s\t%s\tunknown" % (g, r))
        print("[golden-census] %d row(s), %d tile(s), %d unknown row(s)" % (len(agg), len(hits), len(unk)))
    else:
        for g, tile, v, u in sorted(hits, key=lambda h: (-h[2], h[0], h[1])):
            print("%s\t%s\t%d%s\t%s" % (g, tile, v, "?" if u else "", tile_row(tile)))
        print("[golden-census] %d tile(s) in %d row(s), %d unknown row(s)"
              % (len(hits), len({(h[0], tile_row(h[1])) for h in hits}), len(unk)))
    if not hits:
        print("[golden-census] no tile engages %r: a row that does is owed (LAW 9a)" % a.pred)
    return 0


def expect_lines(hits, unk, reason):
    """Expected-moves lines (golden-fleet.py --expect): one per tile, one per unknown row."""
    r = " ".join(reason.split())
    out = ["%s-presentation %s %s" % (g, tile, r) for g, tile, _v, _u in sorted(hits)]
    out += ["%s-presentation %s__ %s" % (g, row, r) for g, row in sorted(unk)]
    return out


def cmd_expect(a):
    """Print expected-moves lines for a predicate, or run the commit step of a query file (--query)."""
    if a.query:
        return expect_query(a)
    if not (a.pred and a.reason and (a.game or a.all)):
        raise Refused("expect takes --game G or --all, a predicate and --reason, or --query FILE")
    registry = load_registry()
    terms = parse_pred(a.pred, registry)
    sources = open_sources(a.store, games_of(a))
    if not sources:
        return 1
    hits, unk = select(sources, terms, registry)
    for ln in expect_lines(hits, unk, a.reason):
        print(ln)
    print("# %d tile(s), %d unknown row(s) from %r" % (len(hits), len(unk), a.pred), file=sys.stderr)
    return 0


# ---- query files -------------------------------------------------------------

def parse_query(path, registry):
    """{"header": [lines], "reason": text, "preds": [(n, games, text, terms)], "hands": [(n, game, sub, missing)]}; n counts body lines."""
    q = {"header": [], "reason": None, "preds": [], "hands": []}
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    q["header"] = [ln for ln in lines if ln.startswith("#")]
    # Line numbers count body lines only, so the stamped header moves none.
    for n, ln in enumerate((ln for ln in lines if not ln.startswith("#")), 1):
        if not ln.strip():
            continue
        f = ln.split("\t")
        kind = f[0].strip()
        if kind == "reason" and len(f) == 2:
            if q["reason"] is not None:
                raise Refused("%s:%d: a second reason line" % (path, n))
            q["reason"] = f[1].strip()
        elif kind == "pred" and len(f) == 3:
            gs = list(GAMES) if f[1].strip() == "all" else [g.strip() for g in f[1].split(",")]
            bad = [g for g in gs if g not in GAMES]
            if bad:
                raise Refused("%s:%d: unknown game %s" % (path, n, ", ".join(bad)))
            q["preds"].append((n, gs, f[2].strip(), parse_pred(f[2], registry)))
        elif kind == "hand" and len(f) in (3, 4):
            if f[1].strip() not in GAMES:
                raise Refused("%s:%d: unknown game %s" % (path, n, f[1].strip()))
            miss = f[3].strip() if len(f) == 4 else ""
            if not re.match(r"^missing:\S+$", miss):
                raise Refused("%s:%d: a hand line names the counter that would select it: "
                              "`missing:<counter>` or `missing:none`" % (path, n))
            q["hands"].append((n, f[1].strip(), f[2].strip(), miss[len("missing:"):]))
        else:
            raise Refused("%s:%d: not a reason, pred or hand line: %r" % (path, n, ln))
    if not q["reason"]:
        raise Refused("%s: no reason line" % path)
    if not q["preds"] and not q["hands"]:
        raise Refused("%s: no pred or hand line" % path)
    return q


def read_fixed(d):
    """{body hash: (name, time)} of a queries directory's fixed.tsv."""
    out = {}
    try:
        with open(os.path.join(d, FIXED_NAME), encoding="utf-8") as f:
            for ln in f:
                p = ln.rstrip("\n").split("\t")
                if len(p) >= 3:
                    out.setdefault(p[1], (p[0], p[2]))
    except OSError:
        pass
    return out


def append_fixed(path):
    """Append a file's body hash to fixed.tsv in its directory; return the line."""
    line = "%s\t%s\t%s\n" % (os.path.basename(path), body_hash(path), utc_now())
    with open(os.path.join(os.path.dirname(os.path.abspath(path)), FIXED_NAME), "a", encoding="utf-8") as f:
        f.write(line)
    return line


def cmd_fix(a):
    """Append each file's body hash to fixed.tsv beside it (the orchestrator, on receipt)."""
    for p in a.files:
        if not os.path.isfile(p):
            raise Refused("%s: no such file" % p)
        print("[golden-census] fixed: %s" % append_fixed(p).rstrip("\n"))
    return 0


def expect_query(a):
    """At the commit: check the file was fixed, stamp it, write <sha>.tiles, print the expect lines, fix the .tiles."""
    registry = load_registry()
    path = a.query
    q = parse_query(path, registry)
    qdir = os.path.dirname(os.path.abspath(path))
    fixed = read_fixed(qdir).get(body_hash(path))
    print("[golden-census] %s: %s" % (path, "fixed %s as %s" % (fixed[1], fixed[0]) if fixed
                                      else "no %s line: post-feedback" % FIXED_NAME))
    games = sorted({g for _n, gs, _t, _terms in q["preds"] for g in gs})
    sources = open_sources(a.store, games) if games else {}
    missing = [g for g in games if g not in sources]
    if missing:
        raise Refused("a pred line names %s, which has no counts source" % ", ".join(missing))
    sha = a.sha or git("rev-parse", "HEAD").strip()
    header = ["# commit %s" % sha] + ["# counts %s %s %s" % (g, sources[g].run_id,
                                                             (sources[g].entry.get("source_tree") or "")[:12])
                                      for g in games]
    with open(path, encoding="utf-8") as f:
        body = [ln for ln in f.read().splitlines() if not ln.startswith("#")]
    with open(path + ".tmp", "w", encoding="utf-8") as f:
        f.write("\n".join(header + body) + "\n")
    os.replace(path + ".tmp", path)
    tiles_out, expect, ntiles, nrows = ["# commit %s" % sha], [], 0, set()
    for n, gs, text, terms in q["preds"]:
        hits, unk = select({g: sources[g] for g in gs}, terms, registry)
        tiles_out += ["pred\t%d\t%s\t%s\t%d%s" % (n, g, t, v, "?" if u else "") for g, t, v, u in sorted(hits)]
        tiles_out += ["unknown\t%d\t%s\t%s" % (n, g, r) for g, r in unk]
        expect += expect_lines(hits, unk, q["reason"])
        ntiles += len(hits)
        nrows |= {(g, tile_row(t)) for g, t, _v, _u in hits}
        print("[golden-census] pred line %d (%s): %d tile(s) in %d row(s), %d unknown row(s)"
              % (n, text, len(hits), len({(g, tile_row(t)) for g, t, _v, _u in hits}), len(unk)))
    for n, g, sub, miss in q["hands"]:
        tiles_out.append("hand\t%d\t%s\t%s\t%s" % (n, g, sub, miss))
        expect.append("%s-presentation %s %s" % (g, sub, " ".join(q["reason"].split())))
    tpath = os.path.join(qdir, "%s.tiles" % sha)
    with open(tpath + ".tmp", "w", encoding="utf-8") as f:
        f.write("\n".join(tiles_out) + "\n")
    os.replace(tpath + ".tmp", tpath)
    append_fixed(tpath)
    expect = list(dict.fromkeys(expect))
    for ln in expect:
        print(ln)
    if a.append:
        with open(a.append, "a", encoding="utf-8") as f:
            f.writelines(ln + "\n" for ln in expect)
    if not q["preds"]:
        print("[golden-census] engagement: none measured (no pred line)")
    else:
        print("[golden-census] engagement: %d tile(s) in %d row(s)%s; wrote %s"
              % (ntiles, len(nrows), "" if ntiles else " (no tile engages: a row is owed, LAW 9a)", tpath))
    return 0


# ---- nearest -----------------------------------------------------------------

def cmd_nearest(a):
    """Print the rows with the highest values of a counter; with none, the rows of its path family."""
    load_registry().live(a.counter)
    sources = open_sources(a.store, [a.game])
    if not sources:
        return 1
    src = sources[a.game]
    agg = {}
    for tile, (_gen, vals) in src.tiles.items():
        v = vals.get(a.counter, 0) + vals.get("backdrop." + a.counter, 0)
        if v:
            agg[tile_row(tile)] = max(agg.get(tile_row(tile), 0), v)
    if agg:
        for r, v in sorted(agg.items(), key=lambda kv: (-kv[1], kv[0]))[:a.top]:
            print("%s\t%s\tmax=%d" % (a.game, r, v))
        return 0
    print("[golden-census] no tile engages %s: a row that does is owed (LAW 9a)" % a.counter)
    fam = a.counter.split(".")[0]
    near = {}
    for tile, (_gen, vals) in src.tiles.items():
        s = sum(v for k, v in vals.items() if k.split(".")[0] == fam)
        if s:
            near[tile_row(tile)] = max(near.get(tile_row(tile), 0), s)
    for r, v in sorted(near.items(), key=lambda kv: (-kv[1], kv[0]))[:a.top]:
        print("%s\t%s\t%s.* max=%d" % (a.game, r, fam, v))
    return 0


# ---- settles -----------------------------------------------------------------

def settle_summary(path):
    """{legs, settles, pass_us, max_us} summed over a census-settle.tsv, or None when it is absent.

    A line is one capture leg: kind, row key, lod-leg, then the process's
    frozen-backdrop settles, their tally pass time in microseconds and the
    longest pass (scripts/presentation-game.sh pg_census_settle).
    """
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError:
        return None
    out = {"legs": 0, "settles": 0, "pass_us": 0.0, "max_us": 0.0}
    for ln in lines:
        f = ln.split("\t")
        if len(f) != 6:
            continue
        out["legs"] += 1
        out["settles"] += int(f[3])
        out["pass_us"] += float(f[4])
        out["max_us"] = max(out["max_us"], float(f[5]))
    return out


def settle_line(summary):
    """One line of a settle summary: legs, settles, total and per-settle pass time, longest pass."""
    n = summary["settles"]
    return ("%d leg(s), %d backdrop settle(s), tally pass %.1f ms total, %.1f us per settle, %.1f us longest"
            % (summary["legs"], n, summary["pass_us"] / 1e3, summary["pass_us"] / n if n else 0.0,
               summary["max_us"]))


def cmd_settles(a):
    """Print each game's frozen-backdrop settle count and tally pass time from its counts source."""
    sources = open_sources(a.store, games_of(a))
    if not sources:
        return 1
    for g, src in sorted(sources.items()):
        s = settle_summary(os.path.join(src.dir, SETTLE_NAME))
        print("%s\t%s" % (g, settle_line(s) if s else "no %s in the entry" % SETTLE_NAME))
    return 0


# ---- score -------------------------------------------------------------------

def read_stream(path):
    """{(game, tile): (verb, digest16)} of a stream.log, the newest attempt per tile."""
    newest = {}
    for ln in open(path, encoding="utf-8"):
        f = ln.split()
        if len(f) < 6 or ln.startswith("#") or f[4] not in VERBS:
            continue
        att = int(f[2]) if f[2].isdigit() else 0
        k = (f[0].rsplit("-", 1)[0], f[5])
        if k not in newest or att >= newest[k][0]:
            newest[k] = (att, f[4], f[6] if len(f) > 6 else "")
    return {k: (v[1], v[2]) for k, v in newest.items()}


def read_tiles_file(path):
    """{"pred": {lineno: {(game, tile)}}, "unknown": {lineno: {(game, row)}}, "hand": [(lineno, game, sub, missing)]} of a .tiles file."""
    out = {"pred": {}, "unknown": {}, "hand": []}
    with open(path, encoding="utf-8") as f:
        for ln in f:
            if ln.startswith("#") or not ln.strip():
                continue
            p = ln.rstrip("\n").split("\t")
            if p[0] == "pred":
                out["pred"].setdefault(int(p[1]), set()).add((p[2], p[3]))
            elif p[0] == "unknown":
                out["unknown"].setdefault(int(p[1]), set()).add((p[2], p[3]))
            elif p[0] == "hand":
                out["hand"].append((int(p[1]), p[2], p[3], p[4] if len(p) > 4 else ""))
    return out


def read_hand_list(path):
    """[(game, substring)] of a hand list (expected-moves lines `<unit> <substring> <reason>`)."""
    out = []
    with open(path, encoding="utf-8") as f:
        for ln in f:
            if not ln.strip() or ln.lstrip().startswith("#"):
                continue
            p = ln.split(None, 2)
            if len(p) >= 2:
                out.append((p[0].rsplit("-", 1)[0], p[1]))
    return out


def rows_matching(stream_tiles, game, sub):
    """{(game, row)} and {(game, tile)} of the streamed tiles of a game whose name holds a substring."""
    tiles = {(g, t) for g, t in stream_tiles if g == game and sub in t}
    return {(g, tile_row(t)) for g, t in tiles}, tiles


def pred_is_hand(text, terms, nrows):
    """Whether a pred line scores as a hand line under --window: 2 rows or fewer, a launch: term or an upper bound."""
    return nrows <= 2 or any(k == "launch" for k, *_ in terms) or any(
        k == "ctr" and op in UPPER for k, _a, op, _b in terms)


class Commit:
    """One commit's query file, its expansion and its hand list, scored against a run."""

    def __init__(self, qdir, sha, suffix, registry, stream_tiles, window):
        """Read <sha>.q<suffix>, <sha>.tiles and <sha>.hand<suffix>; build both sides' rows and tiles."""
        self.sha, self.suffix = sha, suffix
        qpath = os.path.join(qdir, "%s.q%s" % (sha, suffix))
        self.q = parse_query(qpath, registry)
        tpath = os.path.join(qdir, "%s.tiles" % sha)
        if not os.path.isfile(tpath):
            raise Refused("%s has no %s: expect --query did not run at the commit" % (qpath, tpath))
        tl = read_tiles_file(tpath)
        self.counts = {}
        for ln in self.q["header"]:
            p = ln.split()
            if len(p) >= 4 and p[1] == "counts":
                self.counts[p[2]] = p[3]
        self.pred_rows, self.pred_tiles, self.hand_rows, self.hand_tiles = set(), set(), set(), set()
        self.unknown, self.as_hand = set(), []
        for n, _gs, text, terms in self.q["preds"]:
            tiles = tl["pred"].get(n, set())
            unk = tl["unknown"].get(n, set())
            rows = {(g, tile_row(t)) for g, t in tiles}
            self.unknown |= unk
            if window and pred_is_hand(text, terms, len(rows | unk)):
                self.as_hand.append(n)
                self.hand_rows |= rows
                self.hand_tiles |= tiles
            else:
                self.pred_rows |= rows
                self.pred_tiles |= tiles
        for _n, g, sub, _miss in self.q["hands"]:
            r, t = rows_matching(stream_tiles, g, sub)
            self.hand_rows |= r
            self.hand_tiles |= t
        self.list_rows, self.list_tiles = set(), set()
        hpath = os.path.join(qdir, "%s.hand%s" % (sha, suffix))
        self.has_list = os.path.isfile(hpath)
        if self.has_list:
            for g, sub in read_hand_list(hpath):
                r, t = rows_matching(stream_tiles, g, sub)
                self.list_rows |= r
                self.list_tiles |= t
        self.query_rows = self.pred_rows | self.hand_rows
        self.query_tiles = self.pred_tiles | self.hand_tiles
        self.side_hand_rows = self.list_rows | self.hand_rows
        self.named = {a for _n, _g, _t, terms in self.q["preds"] for k, a, *_ in terms if k == "ctr"}
        self.named |= {m for _n, _g, _s, m in self.q["hands"] if m != "none"}


def score_ratio(num, den):
    """num/den to three places, or n/a."""
    return "n/a" if not den else "%.3f" % (num / den)


def miss_class(g, tile, row_held, commits, sources, env):
    """The computed class of a missed moved tile, or `adjudicate` when the inputs do not decide."""
    if env:
        return "environment"
    src = sources.get(g)
    if any((g, tile_row(tile)) in c.unknown for c in commits):
        return "stale"
    if src is None or tile not in src.tiles:
        return "adjudicate"
    gen, vals = src.tiles[tile]
    named = set().union(*[c.named for c in commits]) if commits else set()
    reg = load_registry()
    if tile in src.moved or any(reg.resolve(n) and gen < reg.resolve(n)["since"] for n in named):
        return "stale"
    if row_held:
        return "leg"
    if any(vals.get(n, 0) or vals.get("backdrop." + n, 0) for n in named):
        return "excluded"
    return "uncounted"


def tile_counters(sources, g, tile, limit=8):
    """The first nonzero counters of a tile in its counts source, as text."""
    src = sources.get(g)
    if src is None or tile not in src.tiles:
        return "(no sidecar in the counts source)"
    vals = src.tiles[tile][1]
    return " ".join("%s=%d" % (k, vals[k]) for k in sorted(vals)[:limit])


def batch_env(run_args):
    """True when the run records a commit range none of whose commits touches the renderer."""
    rng = run_args.get("commits")
    if not rng:
        return False
    shas = (git("rev-list", rng, check=False) or "").split()
    return bool(shas) and renderer_commits(shas) == 0


def score_files(qdir, recorded):
    """[(sha, suffix)] of a queries directory's query files, after the hash refusals."""
    fixed = read_fixed(qdir)
    names = sorted(n for n in os.listdir(qdir) if os.path.isfile(os.path.join(qdir, n)))
    for n in names:
        p = os.path.join(qdir, n)
        if n in recorded and file_hash(p) != recorded[n]:
            raise Refused("%s: sha256 differs from the run's run-args.json record" % n)
        if n not in recorded and n != FIXED_NAME:
            raise Refused("%s: not recorded in the run's run-args.json (added after the run started)" % n)
        if n != FIXED_NAME and re.search(r"\.(q2?|hand2?|tiles)$", n) and body_hash(p) not in fixed:
            raise Refused("%s: its hash has no %s line (edited after it was fixed, or never fixed)"
                          % (n, FIXED_NAME))
    return [(m.group(1), m.group(2)) for m in (re.match(r"^([0-9a-f]{7,40})\.q(2?)$", n) for n in names) if m]


def cmd_score(a):
    """Score a full run's query files: moved rows held by each side, selected fraction, misses with class."""
    run_args = read_json(os.path.join(a.run, "run-args.json"))
    if run_args is None:
        raise Refused("%s: no run-args.json" % a.run)
    recorded = run_args.get("queries")
    qdir = os.path.join(a.run, "queries")
    if not recorded or not os.path.isdir(qdir):
        raise Refused("%s records no queries: the run started without --queries; not scored" % a.run)
    stream = read_stream(os.path.join(a.run, "stream.log"))
    moved_tiles = {k for k, (v, _d) in stream.items() if v in MOVED}
    moved_rows = {(g, tile_row(t)) for g, t in moved_tiles}
    streamed_rows = {(g, tile_row(t)) for g, t in stream}
    registry = load_registry()
    files = score_files(qdir, recorded)
    commits = [Commit(qdir, sha, sfx, registry, set(stream), a.window) for sha, sfx in files]
    games = sorted({g for c in commits for g in c.counts} | {g for g, _r in moved_rows})
    sources = open_sources(a.store, games)
    print("[golden-census] score %s: moved rows %d, moved tiles %d, streamed rows %d, query files %d%s"
          % (os.path.basename(os.path.normpath(a.run)), len(moved_rows), len(moved_tiles), len(streamed_rows),
             len(commits), ", --window" if a.window else ""))
    primary = [c for c in commits if not c.suffix]
    late = [c for c in commits if c.suffix]
    for c in commits:
        score_one(c, moved_rows, streamed_rows, primary)
    print("[golden-census] batch (first versions, %d commit(s)):" % len(primary))
    batch = score_side(primary, moved_rows, streamed_rows, a.window)
    if late:
        print("[golden-census] post-feedback versions (scored separately from the first versions):")
        score_side(late, moved_rows, streamed_rows, a.window)
    env = batch_env(run_args)
    held_rows = set().union(*[c.query_rows for c in primary]) if primary else set()
    held_tiles = set().union(*[c.query_tiles for c in primary]) if primary else set()
    unknown = set().union(*[c.unknown for c in primary]) if primary else set()
    for g, t in sorted(moved_tiles):
        row = (g, tile_row(t))
        if (g, t) in held_tiles or row in unknown or (row in held_rows and (g, t) in held_tiles):
            continue
        if row in held_rows:
            cls = miss_class(g, t, True, primary, sources, env)
        elif row in batch["miss_rows"]:
            cls = miss_class(g, t, False, primary, sources, env)
        else:
            continue
        print("  miss %s %s [%s] %s" % (g, t, cls, tile_counters(sources, g, t)))
    drift_report(sources, a.store)
    return 0


def score_one(c, moved_rows, streamed_rows, primary):
    """Print one commit file's numbers."""
    mv = moved_rows - c.unknown
    held = mv & c.query_rows
    sel = (c.query_rows | c.unknown) & streamed_rows | c.unknown
    others = [o for o in primary if o is not c]
    attrib = {"sole": 0, "shared": 0}
    for r in held:
        attrib["shared" if any(r in o.query_rows for o in others) else "sole"] += 1
    print("[golden-census] %s.q%s: reason: %s" % (c.sha[:12], c.suffix, c.q["reason"]))
    print("  moved rows held by pred %d, by hand %d, by the query side %d of %d (recall %s); "
          "selected %d of %d streamed rows (%s); unknown rows %d (%d moved); %s"
          % (len(mv & c.pred_rows), len(mv & c.hand_rows), len(held), len(mv), score_ratio(len(held), len(mv)),
             len(sel), len(streamed_rows), score_ratio(len(sel), len(streamed_rows)), len(c.unknown),
             len(moved_rows & c.unknown), "sole %d, shared %d" % (attrib["sole"], attrib["shared"])))
    if c.as_hand:
        print("  pred lines scored as hand lines (--window): %s" % ", ".join(str(n) for n in c.as_hand))
    if not c.q["preds"]:
        print("  engagement: none measured (no pred line)")
    if c.has_list:
        hh = mv & c.side_hand_rows
        print("  hand side: held %d of %d (recall %s), selected %d"
              % (len(hh), len(mv), score_ratio(len(hh), len(mv)), len(c.side_hand_rows & streamed_rows)))
    for n, g, sub, miss in c.q["hands"]:
        print("  hand line %d: %s %s missing:%s" % (n, g, sub, miss))


def score_side(commits, moved_rows, streamed_rows, window):
    """Print and return the batch's pooled numbers over a set of commit files."""
    q_rows = set().union(*[c.query_rows for c in commits]) if commits else set()
    h_rows = set().union(*[c.side_hand_rows for c in commits]) if commits else set()
    unk = set().union(*[c.unknown for c in commits]) if commits else set()
    mv = moved_rows - unk
    sq = (q_rows & streamed_rows) | unk
    sh = h_rows & streamed_rows
    out = {"Rq": (len(mv & q_rows), len(mv)), "Rh": (len(mv & h_rows), len(mv)),
           "Sq": (len(sq), len(streamed_rows)), "Sh": (len(sh), len(streamed_rows)),
           "miss_rows": mv - q_rows}
    print("  query side: held %d of %d moved rows (Rq %s); selected %d of %d (Sq %s)"
          % (out["Rq"][0], out["Rq"][1], score_ratio(*out["Rq"]), out["Sq"][0], out["Sq"][1],
             score_ratio(*out["Sq"])))
    if window or any(c.has_list for c in commits):
        print("  hand side: held %d of %d moved rows (Rh %s); selected %d of %d (Sh %s)"
              % (out["Rh"][0], out["Rh"][1], score_ratio(*out["Rh"]), out["Sh"][0], out["Sh"][1],
                 score_ratio(*out["Sh"])))
    print("  unknown rows (excluded from both recalls): %d, %d moved" % (len(unk), len(moved_rows & unk)))
    print("  missed rows: %d" % len(out["miss_rows"]))
    if not any(c.q["preds"] for c in commits):
        print("  engagement: none measured (no pred line)")
    return out


def drift_report(sources, store):
    """Per counts source, tiles whose counters changed with the digest unchanged, and the reverse, against the unit's previous entry."""
    for g, src in sorted(sources.items()):
        entries = [d for rid, d in unit_entries(store, src.unit) if rid < src.run_id]
        if not entries:
            print("[golden-census] drift %s: no previous entry" % src.unit)
            continue
        prev_dig = read_digests(entries[0])
        ctr_only, dig_only = [], []
        for t, (gen, vals) in src.tiles.items():
            p = os.path.join(entries[0], t[:-len(".png")] + ".census")
            if t not in prev_dig or not os.path.isfile(p):
                continue
            same_dig = prev_dig[t] == src.digests.get(t)
            same_ctr = read_sidecar(p)[2] == vals
            if same_dig and not same_ctr:
                ctr_only.append(t)
            elif not same_dig and same_ctr:
                dig_only.append(t)
        print("[golden-census] drift %s against %s: counters changed, digest same: %d%s; digest changed, "
              "counters same (candidate uncounted): %d%s"
              % (src.unit, os.path.basename(entries[0]), len(ctr_only),
                 " (first %s)" % sorted(ctr_only)[0] if ctr_only else "", len(dig_only),
                 " (first %s)" % sorted(dig_only)[0] if dig_only else ""))


# ---- digests and sidecar determinism -----------------------------------------

def cmd_digests(a):
    """Write DIR/tile-digests.tsv for every -vk tile of the directory."""
    pb = load_pb()
    tiles = sorted(f for f in os.listdir(a.dir) if f.endswith("-vk.png"))
    digs = pb.digest_files([os.path.join(a.dir, t) for t in tiles])
    out = ["# tile\tdigest16"]
    bad = 0
    for t in tiles:
        d = digs[os.path.join(a.dir, t)]
        if d is None:
            bad += 1
            continue
        out.append("%s\t%s" % (t, d[0][:DIGEST_HEX]))
    path = os.path.join(a.dir, DIGESTS_NAME)
    with open(path + ".tmp", "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    os.replace(path + ".tmp", path)
    print("[golden-census] %s: %d tile(s)%s" % (path, len(out) - 1, ", %d undecodable" % bad if bad else ""))
    return 1 if bad else 0


def sidecar_bytes(d):
    """{sidecar name: bytes} of a directory."""
    out = {}
    for f in sorted(os.listdir(d)):
        if f.endswith("-vk.census"):
            with open(os.path.join(d, f), "rb") as fh:
                out[f] = fh.read()
    return out


def cmd_sidecar_diff(a):
    """Compare sidecars byte for byte: two directories by name, or one directory's repeat copies of each tile."""
    pb = load_pb()
    if a.other:
        x, y = sidecar_bytes(a.dir), sidecar_bytes(a.other)
        pairs = [(n, x[n], y[n]) for n in sorted(set(x) & set(y))]
        only = sorted(set(x) ^ set(y))
    else:
        groups = {}
        for n, b in sidecar_bytes(a.dir).items():
            origin, k = pb.copy_origin(n)
            if k is not None:
                groups.setdefault(origin, []).append((n, b))
        pairs = [(o, g[0][1], b) for o, g in sorted(groups.items()) for _n, b in g[1:]]
        only = []
    diff = [n for n, p, q in pairs if p != q]
    for n in diff:
        print("DIFFER %s" % n)
    for n in only:
        print("ONLY   %s" % n)
    print("[golden-census] sidecar-diff: %d pair(s) compared, %d differ, %d unpaired" % (len(pairs), len(diff), len(only)))
    if not pairs:
        print("[golden-census] VACUOUS: no sidecar pair")
    return 1 if diff or only or not pairs else 0


# ---- main --------------------------------------------------------------------

def main(argv=None):
    """Dispatch a subcommand."""
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("digests")
    p.add_argument("dir")
    p = sub.add_parser("query")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--game", choices=GAMES)
    g.add_argument("--all", action="store_true")
    p.add_argument("pred")
    p.add_argument("--rows", action="store_true")
    p = sub.add_parser("expect")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--game", choices=GAMES)
    g.add_argument("--all", action="store_true")
    p.add_argument("pred", nargs="?")
    p.add_argument("--reason")
    p.add_argument("--query", metavar="FILE")
    p.add_argument("--sha", help="the commit to stamp (default HEAD)")
    p.add_argument("--append", metavar="FILE", help="also append the expect lines to FILE")
    p = sub.add_parser("fix")
    p.add_argument("files", nargs="+")
    p = sub.add_parser("nearest")
    p.add_argument("--game", choices=GAMES, required=True)
    p.add_argument("counter")
    p.add_argument("--top", type=int, default=10)
    p = sub.add_parser("settles")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--game", choices=GAMES)
    g.add_argument("--all", action="store_true")
    p = sub.add_parser("score")
    p.add_argument("--run", required=True, metavar="DIR", help="the full run's local run directory")
    p.add_argument("--window", action="store_true",
                   help="compare with hand lists: narrow, launch: and upper-bound pred lines score as hand lines")
    p = sub.add_parser("sidecar-diff")
    p.add_argument("dir")
    p.add_argument("other", nargs="?")
    for name in ("query", "expect", "nearest", "settles", "score"):
        sub.choices[name].add_argument("--store", metavar="DIR", help="the census store (default <local fleet_root>/census)")
    a = ap.parse_args(argv)
    if getattr(a, "store", "unset") is None:
        a.store = default_store()
    fn = {"digests": cmd_digests, "query": cmd_query, "expect": cmd_expect, "fix": cmd_fix,
          "nearest": cmd_nearest, "settles": cmd_settles, "score": cmd_score,
          "sidecar-diff": cmd_sidecar_diff}[a.cmd]
    try:
        return fn(a)
    except Refused as e:
        print("[golden-census] REFUSED: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
