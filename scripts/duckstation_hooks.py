#!/usr/bin/env python3
"""duckstation_hooks.py - build the oracle reference tree from the pinned upstream commit and the series.

The series (third_party/duckstation-patches/) holds no upstream text: `files/` are whole files of its own,
copied to the same path in the reference tree, and `hooks.txt` lists the lines it inserts into upstream
files, each located by a hash of the upstream lines around the insertion point.

  apply  DS_DIR [--series DIR] [--print-head] [--partial]
         Builds the applied tree on the pin (DS_DIR's index gitlink in the superproject, or --pin), commits it
         with fixed metadata and prints the commit id (the applied head). Without --print-head it also checks
         DS_DIR out at that commit (refused when DS_DIR has changes to tracked files). An insertion whose anchor
         does not match is listed; --partial skips those instead of refusing.
  derive DS_DIR [--series DIR]
         Rewrites hooks.txt from DS_DIR's working tree: every upstream file that differs from the pin must
         differ by inserted lines only; files absent at the pin are copied into files/.
  check  DS_DIR [--series DIR]
         Applies twice into temporary indexes and asserts the same head, and that every upstream line of
         every target file is kept, in order.
  census DS_DIR PATH...
         Counts the lines of the files under PATH that occur as a line of the pin's tree (exact text after
         stripping whitespace and, in a .patch file, the diff marker), by stripped length.

hooks.txt lines:
  pin <commit>                 the pin the anchors were derived at
  file <path>                  the upstream file the insertions below go into
  @ <name> <n> <k> <hash>      an insertion after the k-th of the n consecutive upstream lines whose anchor
                               hash is <hash> (k = 0: before the first); then its lines, each prefixed '+'
Anchor hash: the first 16 hex digits of sha256("openpete-anchor-1\\0" + path + "\\0" + the n lines, each with
its newline). The window is the smallest one before the insertion point (after it at the top of a file)
holding at least MIN_CHARS non-blank characters whose hash occurs once among the file's n-line windows.

Applied head: the commit whose parent is the pin and whose tree is the applied tree, with author and committer
COMMITTER, both dates the pin's committer date and the message MESSAGE. It is a function of the pin and the
applied tree only.

Exit: 0 done; 1 refusal (reason on stderr); 64 usage.
"""
import argparse
import hashlib
import os
import subprocess
import sys
import tempfile

SALT = b"openpete-anchor-1\0"
MIN_CHARS = 48
COMMITTER = ("openpete psxtrace series", "psxtrace-series@openpete.invalid")
MESSAGE = "openpete psxtrace series\n"
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIES = os.path.join(REPO, "third_party", "duckstation-patches")


def die(msg):
    print(f"[ds-hooks] refusing: {msg}", file=sys.stderr)
    sys.exit(1)


def git_env(index=None):
    env = dict(os.environ)
    env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL="/dev/null", LC_ALL="C", TZ="UTC")
    for k in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_AUTHOR_DATE", "GIT_COMMITTER_DATE"):
        env.pop(k, None)
    if index:
        env["GIT_INDEX_FILE"] = index
    return env


def git(ds, *args, data=None, index=None, text=False):
    r = subprocess.run(["git", "-C", ds, "-c", "core.hooksPath=/dev/null", "-c", "commit.gpgSign=false",
                        "-c", "core.autocrlf=false", "-c", "core.safecrlf=false", *args],
                       input=data, capture_output=True, env=git_env(index))
    if r.returncode != 0:
        die(f"git {' '.join(args[:3])} failed: {r.stderr.decode(errors='replace').strip()}")
    return r.stdout.decode() if text else r.stdout


def default_pin(ds):
    """The superproject's index gitlink for DS_DIR, the pinned upstream commit."""
    sup = os.path.dirname(os.path.abspath(ds))
    top = subprocess.run(["git", "-C", sup, "rev-parse", "--show-toplevel"], capture_output=True, text=True)
    if top.returncode != 0:
        return None
    rel = os.path.relpath(os.path.abspath(ds), top.stdout.strip())
    ls = subprocess.run(["git", "-C", top.stdout.strip(), "ls-files", "-s", "--", rel], capture_output=True,
                        text=True).stdout.split()
    return ls[1] if len(ls) >= 2 and ls[0] == "160000" else None


def split_lines(data):
    """The file's lines, each with its newline; the last one may lack it."""
    lines = data.split(b"\n")
    out = [l + b"\n" for l in lines[:-1]]
    if lines[-1]:
        out.append(lines[-1])
    return out


def anchor_hash(path, window):
    return hashlib.sha256(SALT + path.encode() + b"\0" + b"".join(window)).hexdigest()[:16]


def nonblank(window):
    return sum(len(l.strip()) for l in window)


def parse_manifest(path):
    pin, files, cur = None, {}, None
    with open(path, "rb") as f:
        for no, raw in enumerate(f, 1):
            line = raw.rstrip(b"\n")
            if line.startswith(b"+"):
                if cur is None or not files[cur]:
                    die(f"{path}:{no}: inserted line outside an insertion")
                files[cur][-1]["text"].append(line[1:] + b"\n")
            elif not line.strip() or line.startswith(b"#"):
                continue
            else:
                f0 = line.decode().split()
                if f0[0] == "pin":
                    pin = f0[1]
                elif f0[0] == "file":
                    cur = f0[1]
                    files.setdefault(cur, [])
                elif f0[0] == "@" and cur is not None and len(f0) == 5:
                    files[cur].append(dict(name=f0[1], n=int(f0[2]), k=int(f0[3]), hash=f0[4], text=[]))
                else:
                    die(f"{path}:{no}: unreadable line")
    return pin, files


def owned_files(series):
    root = os.path.join(series, "files")
    out = {}
    for d, _, names in os.walk(root):
        for n in names:
            p = os.path.join(d, n)
            out[os.path.relpath(p, root).replace(os.sep, "/")] = p
    return dict(sorted(out.items()))


def insert(path, data, rows, missing):
    """`data` with every row's lines inserted; a row whose anchor does not match exactly once is appended to
    `missing` and skipped."""
    lines = split_lines(data)
    points = []
    windows_by_n = {}
    for order, r in enumerate(rows):
        n = r["n"]
        if n not in windows_by_n:
            windows_by_n[n] = {}
            for s in range(0, len(lines) - n + 1):
                windows_by_n[n].setdefault(anchor_hash(path, lines[s:s + n]), []).append(s)
        hits = windows_by_n[n].get(r["hash"], [])
        if len(hits) != 1:
            missing.append(f"{r['name']} ({path}: anchor matches {len(hits)} places)")
            continue
        points.append((hits[0] + r["k"], order, r["text"]))
    points.sort()
    if points and points[-1][0] == len(lines) and lines and not lines[-1].endswith(b"\n"):
        die(f"{path}: an insertion at the end of a file without a final newline")
    out, j = [], 0
    for i in range(len(lines) + 1):
        while j < len(points) and points[j][0] == i:
            out.extend(points[j][2])
            j += 1
        if i < len(lines):
            out.append(lines[i])
    return b"".join(out)


def build_tree(ds, pin, series, partial=False):
    """Writes the applied tree into DS_DIR's object store; returns (tree id, touched paths)."""
    _, files = parse_manifest(os.path.join(series, "hooks.txt"))
    owned = owned_files(series)
    with tempfile.TemporaryDirectory() as tmp:
        index = os.path.join(tmp, "index")
        git(ds, "read-tree", pin, index=index)
        entries, missing = [], []
        for path, rows in files.items():
            ls = git(ds, "ls-tree", pin, "--", path, text=True).split()
            if not ls:
                die(f"hooks.txt names {path}, absent at the pin {pin}")
            data = git(ds, "cat-file", "blob", ls[2])
            new = insert(path, data, rows, missing)
            entries.append((ls[0], path, new))
        if missing:
            print("[ds-hooks] insertions whose upstream text changed:\n  " + "\n  ".join(missing), file=sys.stderr)
            if not partial:
                die(f"{len(missing)} insertion(s) do not match the pin; re-derive them (series README)")
        for path, src in owned.items():
            if git(ds, "ls-tree", pin, "--", path, text=True).strip():
                die(f"files/{path} exists at the pin; a series-owned file must be new")
            with open(src, "rb") as f:
                entries.append(("100644", path, f.read()))
        info = []
        for mode, path, data in entries:
            blob = git(ds, "hash-object", "-w", "--no-filters", "--stdin", data=data, text=True).strip()
            info.append(f"{mode} {blob}\t{path}\n")
        git(ds, "update-index", "--index-info", data="".join(info).encode(), index=index)
        tree = git(ds, "write-tree", index=index, text=True).strip()
    return tree, [e[1] for e in entries]


def commit(ds, pin, tree):
    date = git(ds, "show", "-s", "--format=%ct", pin, text=True).strip() + " +0000"
    env = git_env()
    env.update(GIT_AUTHOR_NAME=COMMITTER[0], GIT_AUTHOR_EMAIL=COMMITTER[1], GIT_AUTHOR_DATE=date,
               GIT_COMMITTER_NAME=COMMITTER[0], GIT_COMMITTER_EMAIL=COMMITTER[1], GIT_COMMITTER_DATE=date)
    r = subprocess.run(["git", "-C", ds, "-c", "commit.gpgSign=false", "-c", "i18n.commitEncoding=UTF-8",
                        "commit-tree", tree, "-p", pin, "-F", "-"], input=MESSAGE.encode(), capture_output=True,
                       env=env)
    if r.returncode != 0:
        die(f"commit-tree failed: {r.stderr.decode(errors='replace').strip()}")
    return r.stdout.decode().strip()


def resolve_pin(args):
    pin = args.pin or default_pin(args.ds)
    if not pin:
        die("no pin: pass --pin or run inside the superproject")
    return git(args.ds, "rev-parse", "--verify", pin + "^{commit}", text=True).strip()


def cmd_apply(args):
    pin = resolve_pin(args)
    tree, _ = build_tree(args.ds, pin, args.series, args.partial)
    head = commit(args.ds, pin, tree)
    if not args.print_head:
        if git(args.ds, "status", "--porcelain", "--untracked-files=no", text=True).strip():
            die(f"{args.ds} has changes to tracked files")
        git(args.ds, "checkout", "-q", "--detach", head)
    print(head)


def cmd_check(args):
    pin = resolve_pin(args)
    t1, paths = build_tree(args.ds, pin, args.series)
    t2, _ = build_tree(args.ds, pin, args.series)
    if t1 != t2 or commit(args.ds, pin, t1) != commit(args.ds, pin, t2):
        die("two applications differ")
    _, files = parse_manifest(os.path.join(args.series, "hooks.txt"))
    for path in files:
        old = split_lines(git(args.ds, "cat-file", "blob", f"{pin}:{path}"))
        new = split_lines(git(args.ds, "cat-file", "blob", f"{t1}:{path}"))
        j = 0
        for l in new:
            if j < len(old) and l == old[j]:
                j += 1
        if j != len(old):
            die(f"{path}: upstream line {j + 1} is not kept in order")
    print(f"[ds-hooks] check: {len(paths)} files, head {commit(args.ds, pin, t1)}; every upstream line kept")


def choose_anchor(path, lines, p):
    """(start, n) of the window for an insertion before line index p."""
    for n in range(1, len(lines) + 1):
        for s in ([p - n] if p - n >= 0 else []) + ([p] if p + n <= len(lines) and p - n < 0 else []):
            w = lines[s:s + n]
            if nonblank(w) < MIN_CHARS:
                continue
            h = anchor_hash(path, w)
            if sum(1 for t in range(0, len(lines) - n + 1) if anchor_hash(path, lines[t:t + n]) == h) == 1:
                return s, n
    die(f"{path}: no unique anchor for the insertion before line {p + 1}")


def cmd_derive(args):
    import difflib
    pin = resolve_pin(args)
    changed = git(args.ds, "diff", "--name-only", "--no-renames", pin, "--", text=True).split()
    untracked = git(args.ds, "ls-files", "--others", "--exclude-standard", text=True).split()
    out = [f"pin {pin}\n"]
    owned_root = os.path.join(args.series, "files")
    for path in sorted(untracked):
        dst = os.path.join(owned_root, path)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(os.path.join(args.ds, path), "rb") as f, open(dst, "wb") as g:
            g.write(f.read())
    for path in sorted(changed):
        old = split_lines(git(args.ds, "cat-file", "blob", f"{pin}:{path}"))
        with open(os.path.join(args.ds, path), "rb") as f:
            new = split_lines(f.read())
        out.append(f"\nfile {path}\n")
        sm = difflib.SequenceMatcher(None, old, new, autojunk=False)
        k = 0
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag == "equal":
                continue
            if tag != "insert":
                die(f"{path}: upstream lines {i1 + 1}-{i2} are {tag}d; only insertions are allowed")
            s, n = choose_anchor(path, old, i1)
            k += 1
            out.append(f"@ {os.path.basename(path)}:{k} {n} {i1 - s} {anchor_hash(path, old[s:s + n])}\n")
            for l in new[j1:j2]:
                if not l.endswith(b"\n"):
                    die(f"{path}: an inserted line at the end of the file lacks its newline")
                out.append("+" + l.decode("utf-8", "surrogateescape"))
    with open(os.path.join(args.series, "hooks.txt"), "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write("# Insertions into upstream files; scripts/duckstation_hooks.py applies them.\n")
        f.writelines(out)
    print(f"[ds-hooks] derive: {len(changed)} upstream files, {len(untracked)} series-owned files")


def cmd_census(args):
    pin = resolve_pin(args)
    upstream = set()
    listing = git(args.ds, "ls-tree", "-r", pin, text=True).splitlines()
    blobs = [l.split()[2] for l in listing if l.split()[1] == "blob"]
    data = git(args.ds, "cat-file", "--batch", data=("\n".join(blobs) + "\n").encode())
    pos = 0
    while pos < len(data):
        nl = data.index(b"\n", pos)
        size = int(data[pos:nl].split()[2])
        body = data[nl + 1:nl + 1 + size]
        pos = nl + 1 + size + 1
        if b"\0" in body[:8000]:
            continue
        for l in body.split(b"\n"):
            s = l.strip()
            if s:
                upstream.add(s)
    hist, rows = {}, []
    for root in args.paths:
        for d, _, names in os.walk(root):
            for n in sorted(names):
                p = os.path.join(d, n)
                with open(p, "rb") as f:
                    content = f.read().split(b"\n")
                in_hunk = False
                for no, l in enumerate(content, 1):
                    if p.endswith(".patch"):
                        if l.startswith(b"@@"):
                            in_hunk = True
                            continue
                        if l.startswith(b"diff --git"):
                            in_hunk = False
                        if not in_hunk or not l[:1] in (b" ", b"-", b"+"):
                            continue
                        marker, l = l[:1], l[1:]
                    elif p.endswith("hooks.txt"):
                        if not l.startswith(b"+"):
                            continue
                        marker, l = b"+", l[1:]
                    else:
                        marker = b"="
                    s = l.strip()
                    if s and s in upstream:
                        hist[(marker, len(s))] = hist.get((marker, len(s)), 0) + 1
                        rows.append((len(s), os.path.relpath(p, root), no, marker.decode(), s.decode(errors="replace")))
    for t in (1, 20, 30, 40):
        by = {}
        for (m, ln), c in hist.items():
            if ln >= t:
                by[m] = by.get(m, 0) + c
        print(f"matches of {t}+ characters: " + ", ".join(f"{m.decode()!r} {c}" for m, c in sorted(by.items())) +
              f"; total {sum(by.values())}")
    if args.list is not None:
        for ln, p, no, m, s in sorted(rows, key=lambda r: (r[1], r[2])):
            if ln >= args.list:
                print(f"{p}:{no}\t{m}\t{s}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("apply", "derive", "check", "census"):
        p = sub.add_parser(name)
        p.add_argument("ds")
        p.add_argument("--series", default=SERIES)
        p.add_argument("--pin")
        if name == "apply":
            p.add_argument("--print-head", action="store_true")
            p.add_argument("--partial", action="store_true",
                           help="skip insertions whose anchor does not match (for re-deriving after a pin bump)")
        if name == "census":
            p.add_argument("paths", nargs="+")
            p.add_argument("--list", type=int, metavar="LEN", help="list every match of LEN+ characters")
    args = ap.parse_args()
    {"apply": cmd_apply, "derive": cmd_derive, "check": cmd_check, "census": cmd_census}[args.cmd](args)


if __name__ == "__main__":
    main()
