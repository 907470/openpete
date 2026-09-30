#!/usr/bin/env python3
"""Fence: no PSX RAM dump is ever committed.

A repository that carries game bytes cannot be published. A dump reaches
the tree three ways, each closed here by content, so a renamed file is
still caught:

  - a .psxtrace whose header flags carry FLAG_HAS_RAM (per-record 2 MB
    payloads; tools/psxtrace_format.py);
  - a raw 2 MB RAM image (`--ram-dump-output`, `-dump-ram-at-anchor`,
    `check_ram_diff.sh --keep`, the PCSX-Redux `.seed.ram` sibling; the
    `.seed.cpu` register block is banned with it);
  - a PS-X EXE.

A file is BANNED when any rule holds:
  name    basename matches *.ram, *.seed.ram, *.seed.cpu, ram-*.bin,
          *-ram-dump*  (source/text extensions exempt: the name rule is a
          backstop for the content rules, and scripts are named after the
          dumps they write);
  size    exactly 0x200000 or 0x200000 + 528 bytes;
  header  first 8 bytes "PSXTRACE" and u32 flags at offset 20 has bit 1;
  header  first 8 bytes "PS-X EXE".

Modes (argv[1]):
  tree [ROOT]   Scan `git ls-files` of ROOT (default: this repo). Runs the
                self-test first and refuses to report PASS unless both
                negative controls were refused. Exit 0 clean,
                1 banned file present or self-test failed. ctest `no_ram_dumps`.
  check PATH... Classify the named files. Exit 0 clean, 1 banned. Used by
                the capture scripts on their output.
  self-test     Negative controls only: a 2 MB zero file, a hash-only
                .psxtrace renamed to .png with FLAG_HAS_RAM set, a PS-X EXE
                header; and a positive control (hash-only trace) that must
                pass. Exit 0 all controls behaved.

Declared escape: none. A dump never belongs in the repo.
"""
import fnmatch
import os
import shlex
import struct
import subprocess
import sys
import tempfile

RAM_SIZE = 0x200000
CPU_BLOCK = 528
BANNED_SIZES = (RAM_SIZE, RAM_SIZE + CPU_BLOCK)
NAME_GLOBS = ("*.ram", "*.seed.ram", "*.seed.cpu", "ram-*.bin", "*-ram-dump*")
# Extensions the name rule skips: text that names a dump is not a dump.
SOURCE_EXTS = (".lua", ".py", ".sh", ".bash", ".c", ".h", ".cpp", ".hpp",
               ".md", ".txt", ".toml", ".cmake", ".json", ".yaml", ".yml",
               ".rst", ".csv", ".tex", ".html")
PSXTRACE_MAGIC = b"PSXTRACE"
PSXTRACE_FLAGS_OFF = 20
FLAG_HAS_RAM = 1 << 1
PSX_EXE_MAGIC = b"PS-X EXE"
HEADER_BYTES = 32


def classify(name, size, header):
    """Return the violated rule for (basename, byte size, first 32 bytes), or None."""
    base = os.path.basename(name)
    if not base.lower().endswith(SOURCE_EXTS):
        for g in NAME_GLOBS:
            if fnmatch.fnmatch(base, g):
                return "name matches %s" % g
    if size in BANNED_SIZES:
        return "size is 0x%X (a PSX RAM image%s)" % (
            size, " + 528-byte register block" if size != RAM_SIZE else "")
    if header.startswith(PSXTRACE_MAGIC) and len(header) >= PSXTRACE_FLAGS_OFF + 4:
        flags = struct.unpack_from("<I", header, PSXTRACE_FLAGS_OFF)[0]
        if flags & FLAG_HAS_RAM:
            return "PSXTRACE header carries FLAG_HAS_RAM (per-record RAM payloads)"
    if header.startswith(PSX_EXE_MAGIC):
        return "PS-X EXE header"
    return None


def classify_file(path):
    try:
        size = os.path.getsize(path)
        with open(path, "rb") as fh:
            header = fh.read(HEADER_BYTES)
    except OSError:
        return None
    return classify(path, size, header)


def git(repo, *args, check=False):
    r = subprocess.run(["git", "-C", repo] + list(args),
                       capture_output=True, timeout=60)
    if check and r.returncode != 0:
        raise RuntimeError(r.stderr.decode(errors="replace").strip())
    return r


def classify_index_blob(repo, relpath):
    """Classify the staged blob of relpath (not the working-tree file)."""
    r = git(repo, "rev-parse", "-q", "--verify", ":" + relpath)
    if r.returncode != 0:
        return None
    sha = r.stdout.decode().strip()
    size = int(git(repo, "cat-file", "-s", sha, check=True).stdout.decode().strip())
    p = subprocess.Popen(["git", "-C", repo, "cat-file", "blob", sha],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    header = p.stdout.read(HEADER_BYTES)
    p.stdout.close()
    p.terminate()
    p.wait(timeout=10)
    return classify(relpath, size, header)


# ---------------------------------------------------------------- hook mode

def segments(cmd):
    try:
        toks = shlex.split(cmd)
    except ValueError:
        toks = cmd.split()
    seg = []
    for t in toks:
        if t in ("&&", "||", ";", "|", "&") or t.endswith((";", "|")):
            if seg:
                yield seg
            seg = []
        else:
            seg.append(t)
    if seg:
        yield seg


def git_invocation(toks):
    """(repo_dir_override, subcommand, args) for a segment invoking git, else None."""
    if "git" not in toks:
        return None
    rest = toks[toks.index("git") + 1:]
    cdir = None
    i = 0
    while i < len(rest):
        if rest[i] == "-C" and i + 1 < len(rest):
            cdir = rest[i + 1]
            i += 2
        elif rest[i] == "-c" and i + 1 < len(rest):
            i += 2
        elif rest[i].startswith("-"):
            i += 1
        else:
            break
    if i >= len(rest):
        return None
    return cdir, rest[i], rest[i + 1:]


def would_stage(repo, paths):
    """Files `git add paths` would put in the index (tracked + untracked, not ignored)."""
    r = git(repo, "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", *paths)
    if r.returncode != 0:
        return []
    return [p.decode() for p in r.stdout.split(b"\0") if p]


def check_command(cmd, cwd):
    cur = cwd or os.getcwd()
    for toks in segments(cmd):
        if toks and toks[0] == "cd" and len(toks) > 1:
            cur = os.path.normpath(os.path.join(cur, os.path.expanduser(toks[1])))
            continue
        inv = git_invocation(toks)
        if not inv:
            continue
        cdir, sub, args = inv
        repo = os.path.normpath(os.path.join(cur, cdir)) if cdir else cur
        if git(repo, "rev-parse", "--show-toplevel").returncode != 0:
            continue
        top = git(repo, "rev-parse", "--show-toplevel").stdout.decode().strip()
        if sub == "add":
            paths = [a for a in args if not a.startswith("-")]
            if not paths:
                continue
            for rel in would_stage(repo, paths):
                reason = classify_file(os.path.join(top, rel))
                if reason:
                    return "git add would stage %s: %s" % (rel, reason)
        elif sub == "commit":
            r = git(repo, "diff", "--cached", "--name-only", "-z", "--diff-filter=AMR")
            for rel in (p.decode() for p in r.stdout.split(b"\0") if p):
                reason = classify_index_blob(top, rel)
                if reason:
                    return "index holds %s: %s" % (rel, reason)
    return None


# ---------------------------------------------------------- scan / self-test

def scan_tree(root):
    r = git(root, "ls-files", "-z", check=True)
    top = git(root, "rev-parse", "--show-toplevel", check=True).stdout.decode().strip()
    hits = []
    n = 0
    for rel in (p.decode() for p in r.stdout.split(b"\0") if p):
        full = os.path.join(top, rel)
        if not os.path.isfile(full):
            continue  # submodule gitlink or deleted in the working tree
        n += 1
        reason = classify_file(full)
        if reason:
            hits.append((rel, reason))
    return n, hits


def self_test(verbose=True):
    """Negative controls must be refused; the positive control must pass."""
    ok = True
    with tempfile.TemporaryDirectory() as d:
        def put(name, data):
            p = os.path.join(d, name)
            with open(p, "wb") as fh:
                fh.write(data)
            return p

        def hdr(flags):
            return struct.pack("<8sIIIIIII", PSXTRACE_MAGIC, 2, 1, RAM_SIZE, flags, 1, 2, 0) + b"\x01a"

        cases = [
            ("2 MB zero file renamed .bin", put("frame.bin", b"\0" * RAM_SIZE), True),
            ("2 MB + 528 zero file", put("seed.dat", b"\0" * (RAM_SIZE + CPU_BLOCK)), True),
            ("has_ram trace renamed .png", put("shot.png", hdr(FLAG_HAS_RAM) + b"\0" * 64), True),
            ("PS-X EXE renamed .dat", put("x.dat", PSX_EXE_MAGIC + b"\0" * 2040), True),
            ("name rule *.seed.cpu", put("golden.seed.cpu", b"\0" * 16), True),
            ("hash-only trace (positive control)", put("golden.bin", hdr(0) + b"\0" * 64), False),
            ("source named after a dump", put("pcsx-redux-ram-dump.lua", b"-- lua\n"), False),
            ("this fence's own file name", put("no-ram-dump.py", b"# py\n"), False),
        ]
        for label, path, must_refuse in cases:
            reason = classify_file(path)
            refused = reason is not None
            good = refused == must_refuse
            ok &= good
            if verbose:
                print("  [%s] %-38s %s" % ("ok" if good else "BAD", label,
                                           reason or "clean"))
    return ok


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "tree"
    here = os.path.dirname(os.path.abspath(__file__))
    repo_default = os.path.normpath(os.path.join(here, "..", ".."))

    if mode == "self-test":
        print("no-ram-dump self-test:")
        sys.exit(0 if self_test() else 1)

    if mode == "check":
        bad = [(p, classify_file(p)) for p in sys.argv[2:]]
        bad = [(p, r) for p, r in bad if r]
        for p, r in bad:
            print("no-ram-dump: BANNED %s: %s" % (p, r), file=sys.stderr)
        sys.exit(1 if bad else 0)

    if mode == "tree":
        root = sys.argv[2] if len(sys.argv) > 2 else repo_default
        print("no-ram-dump self-test:")
        if not self_test():
            print("no-ram-dump: self-test FAILED; the scan verdict is void", file=sys.stderr)
            sys.exit(1)
        n, hits = scan_tree(root)
        for rel, reason in hits:
            print("no-ram-dump: BANNED %s: %s" % (rel, reason), file=sys.stderr)
        if n == 0:
            print("no-ram-dump: zero files scanned (VACUOUS)", file=sys.stderr)
            sys.exit(1)
        print("no-ram-dump: %d tracked files scanned, %d banned" % (n, len(hits)))
        sys.exit(1 if hits else 0)

    print("no-ram-dump: unknown mode %r (check | self-test | tree [ROOT])" % mode, file=sys.stderr)
    sys.exit(64)

if __name__ == "__main__":
    main()
