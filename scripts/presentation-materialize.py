#!/usr/bin/env python3
"""presentation-materialize.py -- rebuild the blessed revision of presentation
golden rows and re-capture their reference tiles.

Reached as `tests/harness/check_presentation_golden.sh --materialize-ref` and
from `presentation-blessed.py golden-at --materialize`. A manual adjudication
command: refused under ctest, whose presentation row cannot fit a build plus a
recapture in its timeout.

    presentation-materialize.py [--game G] [--tier T] [--at REV]
        [--from-archive] [--measure] [--keep-worktree] [--stop-after STEP]
        [--log-dir DIR] <tile>...

Rows are read from the tier's blessed.tsv in the working tree, or as committed
at --at REV. Per blessed revision the rows name:

 1. disc: the main checkout's disc .cue and the .bin the manifest header names
    are hashed and compared against the header; an absent file, a header that
    records no hash, or a differing hash refuses, naming the file.
 2. worktree: a throwaway worktree at the revision from worktree-setup.sh
    --no-links (under --from-archive, `git worktree add` against the archive
    golden-ref.state names, submodules fetched from their remotes). Every
    untracked build input `worktree-setup.sh --list-links` names, except the
    oracle goldens and the DuckStation trees, must be absent there: a link
    refuses. The game's five inputs are then created as real paths below.
 3. decomp inputs: the decomp's own submodules (tools/maspsx) as worktrees
    of the main checkout's nested module repositories at the decomp's pins,
    and the PSY-Q SDK files the decomp ignores (psyq/include, psyq/lib),
    copied from $OPENPETE_PSYQ_DIR or else the main checkout's decomp psyq/;
    then the disc .cue and .bin into the decomp's build directory.
 4. decomp: re-make the decomp in its own pinned image (the decomp's
    Dockerfile, the tag its docker_env.sh names; the image is built when
    absent) with the decomp tree bind-mounted, by the decomp's documented
    build command, then `sha256sum -c sha256sum.txt`; on a host with no
    container engine, the same command on the host.
 5. psxrecomp: configure and build third_party/psxrecomp/recompiler into the
    worktree's third_party/psxrecomp/build on the host.
 6. regen: scripts/regen-manifest.sh, then scripts/regen-recomp.sh, in the
    worktree on the host (the AST rewriter pointed at the worktree's decomp,
    see rewriter_decomp).
 7. identity: the recomp input digest and the generated-tree digest of the
    worktree against the row's columns; a mismatch refuses.
 8. build: a fresh configure and engine build in the capture image
    (scripts/golden-capture-container.sh with OPENPETE_CAPTURE_TREE), into
    build/linux-release-container, with the linux-release-container preset's
    settings spelled out, since a past revision need not carry the preset.
 9. capture: every bk2 corpus row and freecam row the tiles belong to, one
    harness run per row with that row's name as the filter and the LOD modes
    the tiles carry, in the capture image.
10. compare: each tile's verdict digest against its row; equal files the tile
    into the store at that digest, unequal files nothing and names the row,
    its revision and both digests.

--measure additionally re-makes a copy of the decomp in the same
environment (determinism), compares the re-make's PSX.EXE, overlays and
psx.map and the regens' globals.toml and symbols.toml byte-for-byte against
the main checkout's copies, repeats the re-make on the host when the image's
outputs differ from the main checkout's, and writes `decomp-reproducibility-<game>=pass|fail
<details>` plus the aggregate `decomp-reproducibility=` (pass only when every
game's line passes) into golden-ref.state beside the store. In this mode the
disc check runs after the regens, so the decomp measurement stands without a
disc. --stop-after ends a run after the named step (disc, worktree, decomp,
psxrecomp, regen, identity, build).

Every step's wall time is printed and written to <log-dir>/timings.tsv.

Exit: 0 every row reproduced (or --stop-after reached); 1 a row not
reproduced, a refusal, or a step that could not complete; 2 usage.
"""
import argparse
import contextlib
import hashlib
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "presentation_blessed", os.path.join(HERE, "presentation-blessed.py"))
pb = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pb)

ROOT = pb.repo_root()
STEPS = ("disc", "worktree", "decomp", "psxrecomp", "regen", "identity",
         "build")
EXCLUDED_LINK_RE = re.compile(r"^(tests/golden/[^/]+/psxtrace-|"
                              r"third_party/duckstation/)")
CTEST_MARKERS = ("CTEST_INTERACTIVE_DEBUG_MODE", "DASHBOARD_TEST_FROM_CTEST")


class Refusal(Exception):
    """A step that could not complete or a refusal rule that fired."""


def log(msg):
    """Print one driver line."""
    print("[materialize] " + msg, flush=True)


def main_checkout():
    """The main checkout: the parent of the common git directory."""
    common = pb.git("rev-parse", "--path-format=absolute",
                    "--git-common-dir").strip()
    return os.path.dirname(common.rstrip("/"))


def setup_script():
    """The worktree setup script named by OPENPETE_WORKTREE_SETUP."""
    p = os.environ.get("OPENPETE_WORKTREE_SETUP") or os.path.join(
        os.path.dirname(main_checkout()), "worktree-setup.sh")
    if not os.path.isfile(p):
        raise Refusal("no worktree-setup.sh at %s (set OPENPETE_WORKTREE_SETUP)"
                      % p)
    return p


def container_engine():
    """podman or docker, as scripts/release-container.sh selects one."""
    e = os.environ.get("OPENPETE_CONTAINER_ENGINE")
    if e:
        return e
    for c in ("podman", "docker"):
        if shutil.which(c):
            return c
    return None


class Timer:
    """Wall time per step, printed and written as TSV."""

    def __init__(self, log_dir):
        self.rows = []
        self.log_dir = log_dir

    @contextlib.contextmanager
    def step(self, name):
        """Time one step; a Refusal marks it refused and propagates."""
        t0 = time.monotonic()
        log("step %s ..." % name)
        status = "ok"
        try:
            yield
        except Refusal:
            status = "refused"
            raise
        except Exception:                               # noqa: BLE001
            status = "error"
            raise
        finally:
            dt = time.monotonic() - t0
            self.rows.append((name, dt, status))
            log("step %s %s in %.1f s" % (name, status, dt))
            self.write()

    def write(self):
        """Write timings.tsv."""
        os.makedirs(self.log_dir, exist_ok=True)
        with open(os.path.join(self.log_dir, "timings.tsv"), "w") as fh:
            fh.write("step\tseconds\tstatus\n")
            for n, dt, st in self.rows:
                fh.write("%s\t%.1f\t%s\n" % (n, dt, st))


def run(cmd, cwd=None, env=None, logf=None, what=None):
    """Run a command, its output appended to logf; a non-zero exit is a
    Refusal naming what."""
    with open(logf, "a") if logf else open(os.devnull, "w") as out:
        out.write("$ %s\n" % " ".join(cmd))
        out.flush()
        p = subprocess.run(cmd, cwd=cwd, env=env, stdout=out,
                           stderr=subprocess.STDOUT)
    if p.returncode != 0:
        raise Refusal("%s exited %d (log: %s)" % (what or cmd[0],
                                                   p.returncode, logf))


# ---- rows and header ---------------------------------------------------------

def load_rows(game, tier, at, tiles):
    """(manifest, rows) for the named tiles, read from the working tree or
    from the manifest committed at `at`."""
    mpath = pb.manifest_path(game, tier)
    if at:
        text = pb.git("show", "%s:%s" % (at, mpath), check=False)
        if text is None:
            raise Refusal("%s carries no %s" % (at, mpath))
    else:
        with open(os.path.join(ROOT, mpath)) as fh:
            text = fh.read()
    m = pb.parse_manifest(text, mpath)
    by_tile = {r["tile"]: r for r in m.rows}
    missing = [t for t in tiles if t not in by_tile]
    if missing:
        raise Refusal("no row named %s in %s%s" % (
            missing[0], mpath, " at " + at if at else ""))
    return m, [by_tile[t] for t in tiles]


def header_hash(m, key):
    """(sha256, path) of a disc header field: the path is repo-relative
    (the file the engine loads) or, in a header written before paths were
    recorded, a basename beside the cue. A field that records no hash
    refuses, naming the file."""
    val = m.header.get(key, "absent")
    f = val.split(None, 1)
    if len(f) != 2 or not pb.DIGEST_RE.match(f[0]):
        raise Refusal("the manifest header records no hash for the disc %s "
                      "(`%s: %s`); only a disc whose hash the header records "
                      "is carried in" % (key.split("-")[1], key, val))
    return f[0], f[1]


def disc_check(game, rev, m, main, from_archive=False):
    """Hash the main checkout's .cue and the .bin the header names against
    the header. Returns [(source path, basename)]."""
    repo, sha = None, rev
    if from_archive:
        arch = pb.Archive()
        repo, sha = arch.path, arch.rev(rev) or rev
        arch.close()
    conf = pb.git("show", "%s:games/%s/oracle.conf" % (sha, game), cwd=repo,
                  check=False) or ""
    mm = re.search(r'^DISC="?([^"\n]+)"?', conf, re.M)
    if not mm:
        raise Refusal("games/%s/oracle.conf at %s names no DISC"
                      % (game, rev[:12]))
    cue = os.path.join(main, mm.group(1))
    out = []
    for key, path in (("disc-cue", cue), ("disc-bin", None)):
        want, base = header_hash(m, key)
        if not path:
            path = (os.path.join(main, base) if "/" in base
                    else os.path.join(os.path.dirname(cue), base))
        if not os.path.isfile(path):
            raise Refusal("disc file %s is absent from the main checkout"
                          % path)
        got = pb.sha256_file(path)
        if got != want:
            raise Refusal("disc file %s hashes %s, the header records %s"
                          % (path, got, want))
        out.append((path, os.path.basename(path)))
    return out


# ---- worktree ----------------------------------------------------------------

def make_worktree(rev, name, from_archive, logf):
    """Create the throwaway worktree. Returns its path."""
    main = main_checkout()
    wt = os.path.join(main, "notes", "worktrees", name)
    if not from_archive:
        env = dict(os.environ, OPENPETE_ROOT=main)
        run(["bash", setup_script(), name, "--ref", rev, "--no-links",
             "--no-configure"], env=env, logf=logf,
            what="worktree-setup.sh --no-links")
        return wt
    arch = pb.read_state().get("archive")
    if not arch or not os.path.isdir(arch):
        raise Refusal("--from-archive: golden-ref.state names no archive "
                      "(%s)" % pb.state_path())
    arch_r = pb.Archive()
    sha = arch_r.rev(rev)
    arch_r.close()
    if not sha:
        raise Refusal("--from-archive: %s does not resolve in %s"
                      % (rev, arch))
    run(["git", "-C", arch, "worktree", "add", "-q", "--detach", wt, sha],
        logf=logf, what="git worktree add from the archive")
    subs = pb.git("config", "-f", ".gitmodules", "--get-regexp",
                  r"^submodule\..*\.path$", cwd=wt) or ""
    for line in subs.splitlines():
        s = line.split()[1]
        if s == "third_party/duckstation":
            continue
        run(["git", "-C", wt, "submodule", "update", "--init", "--", s],
            logf=logf, what="submodule update --init %s" % s)
    return wt


def remove_worktree(wt, name, from_archive, logf):
    """Tear the throwaway worktree down."""
    if not wt or not os.path.isdir(wt):
        return
    if from_archive:
        arch = pb.read_state().get("archive")
        shutil.rmtree(wt, ignore_errors=True)
        subprocess.run(["git", "-C", arch, "worktree", "prune"])
        return
    td = os.path.join(os.path.dirname(setup_script()), "worktree-teardown.sh")
    run(["bash", td, name, "--force"], env=dict(os.environ,
                                                OPENPETE_ROOT=main_checkout()),
        logf=logf, what="worktree-teardown.sh")


def input_paths(game):
    """The untracked build inputs worktree-setup.sh links, minus the
    oracle goldens and the DuckStation trees. Returns (all, this game's)."""
    p = subprocess.run(["bash", setup_script(), "--list-links"],
                       env=dict(os.environ, OPENPETE_ROOT=main_checkout()),
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise Refusal("worktree-setup.sh --list-links exited %d"
                      % p.returncode)
    paths = [ln.strip() for ln in p.stdout.splitlines() if ln.strip()]
    paths = [x for x in dict.fromkeys(paths) if not EXCLUDED_LINK_RE.match(x)]
    mine = [x for x in paths if x.startswith("games/%s/" % game)
            or not x.startswith("games/")]
    return paths, mine


def refuse_links(wt, paths):
    """Refuse when any input path in the worktree is a symbolic link or
    resolves outside it."""
    real_wt = os.path.realpath(wt)
    for rel in paths:
        ap = os.path.join(wt, rel)
        if os.path.islink(ap):
            raise Refusal("%s is a link (to %s); a materialize never builds "
                          "through a link into the main checkout"
                          % (ap, os.readlink(ap)))
        if os.path.lexists(ap) and not os.path.realpath(ap).startswith(
                real_wt + os.sep):
            raise Refusal("%s resolves outside the worktree" % ap)


# ---- decomp, psxrecomp, regens ----------------------------------------------

def decomp_image(decomp):
    """(tag, workdir) of a decomp's pinned image, read from its
    docker_env.sh and Dockerfile."""
    with open(os.path.join(decomp, "docker_env.sh")) as fh:
        tag = re.search(r"build\s+-t\s+(\S+)", fh.read())
    with open(os.path.join(decomp, "Dockerfile")) as fh:
        wd = re.search(r"^WORKDIR\s+(\S+)", fh.read(), re.M)
    if not tag or not wd:
        raise Refusal("%s: no image tag in docker_env.sh or no WORKDIR in "
                      "the Dockerfile" % decomp)
    return tag.group(1), wd.group(1)


def decomp_build_cmd(decomp):
    """The decomp's documented build command: `python3 ./build.py` where
    the decomp carries one (its Makefile is marked untested there), else
    `make all` after `make create_dirs`, whose directories `all` does not
    order before the assembler objects under -j."""
    if os.path.isfile(os.path.join(decomp, "build.py")):
        return "python3 ./build.py"
    return "make create_dirs >/dev/null && make all -j%d" % (os.cpu_count()
                                                             or 4)


def remake_in(decomp, pre, logf):
    """Clean the decomp's build outputs (keeping carried-in disc files)
    and run its build command, in a container when pre names one, else on
    the host. Returns the exit status."""
    b = os.path.join(decomp, "build")
    for sub in ("asm", "src", "wad", "psx.elf", "PSX.EXE", "psx.map"):
        p = os.path.join(b, sub)
        if os.path.isdir(p) and not os.path.islink(p):
            shutil.rmtree(p)
        elif os.path.lexists(p):
            os.unlink(p)
    with open(logf, "a") as out:
        return subprocess.run(pre + ["bash", "-c", decomp_build_cmd(decomp)],
                              cwd=decomp, stdout=out,
                              stderr=subprocess.STDOUT).returncode


def sha256sum_check(decomp, pre, logf):
    """`sha256sum -c` over the decomp's sha256sum.txt. Returns (ok, text).

    The list is fed on stdin with each `build.wad/` path spelled
    `build/wad/`: spyro-3's list names 19 cutscene overlays under
    `build.wad/`, a path its build never writes, so the check could not
    read them; the rewrite is counted in the text."""
    with open(os.path.join(decomp, "sha256sum.txt")) as fh:
        listing = fh.read()
    fixed, nfix = re.subn(r"^(\S+\s+\*?)build\.wad/", r"\1build/wad/",
                          listing, flags=re.M)
    cmd = pre[:2] + ["-i"] + pre[2:] if pre else []
    p = subprocess.run(cmd + ["sha256sum", "-c", "-"], cwd=decomp,
                       input=fixed, capture_output=True, text=True)
    with open(logf, "a") as fh:
        fh.write(p.stdout + p.stderr)
    lines = p.stdout.splitlines()
    bad = [ln for ln in lines if not ln.endswith(": OK")]
    note = "-%d-paths-respelled" % nfix if nfix else ""
    if p.returncode == 0:
        return True, "ok-%d-files%s" % (len(lines), note)
    return False, "FAILED-%d-of-%d%s:%s" % (len(bad), len(lines), note,
                                           ";".join(bad[:2]))


def decomp_outputs(decomp):
    """{relative path: sha256} of the re-make's outputs: PSX.EXE, psx.map
    and every overlay."""
    b = os.path.join(decomp, "build")
    rels = ["PSX.EXE", "psx.map"]
    wad = os.path.join(b, "wad")
    if os.path.isdir(wad):
        rels += ["wad/" + f for f in sorted(os.listdir(wad))
                 if f.endswith(".ovl")]
    return {r: (pb.sha256_file(os.path.join(b, r))
                if os.path.isfile(os.path.join(b, r)) else "absent")
            for r in rels}


def container_prefix(decomp):
    """(engine run prefix, environment description) for the decomp's
    pinned image, building the image when absent; ([], "host") without a
    container engine."""
    engine = container_engine()
    if engine is None:
        return [], "host"
    tag, wd = decomp_image(decomp)
    if subprocess.run([engine, "image", "inspect", tag],
                      capture_output=True).returncode != 0:
        ctx = os.path.join(os.path.dirname(os.path.abspath(decomp)),
                           ".decomp-image-ctx")
        os.makedirs(ctx, exist_ok=True)
        shutil.copyfile(os.path.join(decomp, "Dockerfile"),
                        os.path.join(ctx, "Dockerfile"))
        p = subprocess.run([engine, "build", "-t", tag, "-f",
                            os.path.join(ctx, "Dockerfile"), ctx],
                           capture_output=True, text=True)
        shutil.rmtree(ctx, ignore_errors=True)
        if p.returncode != 0:
            raise Refusal("%s build %s exited %d" % (engine, tag,
                                                     p.returncode))
    iid = subprocess.run([engine, "image", "inspect", "--format", "{{.Id}}",
                          tag], capture_output=True, text=True).stdout.strip()
    user = (["--userns=keep-id"] if engine == "podman"
            else ["--user", "%d:%d" % (os.getuid(), os.getgid())])
    return ([engine, "run", "--rm", "-v", "%s:%s" % (decomp, wd)] + user
            + ["-w", wd, tag], "image:%s@%s" % (tag, iid[:12]))


def remake_decomp(decomp, timer, logf, meas):
    """Re-make the decomp in its pinned image (host fallback) and run its
    checksum check. Records env, sha256sum and outputs in meas."""
    with timer.step("decomp-image"):
        pre, env_desc = container_prefix(decomp)
    meas["env"] = env_desc
    with timer.step("decomp-make"):
        rc = remake_in(decomp, pre, logf)
    with timer.step("decomp-sha256sum"):
        ok, text = sha256sum_check(decomp, pre, logf)
    meas["sha256sum"] = text
    meas["outputs"] = decomp_outputs(decomp)
    log("decomp re-made in %s (build exit %d); sha256sum -c: %s"
        % (env_desc, rc, text))
    if not ok:
        tail = ""
        with open(logf) as fh:
            errs = [ln.strip() for ln in fh if "rror" in ln]
        tail = errs[-1] if errs else ""
        raise Refusal("the decomp re-make fails its own checksum check (%s)%s"
                      % (text, "; last error: " + tail if tail else ""))
    return pre, env_desc


def remake_copy(decomp, where, pre_fn, logf):
    """Re-make a copy of the decomp tree (build outputs excluded) at
    `where`. Returns ({output: sha256}, sha256sum text, environment)."""
    shutil.rmtree(where, ignore_errors=True)
    shutil.copytree(decomp, where, symlinks=True,
                    ignore=lambda d, names: ["build"] if d == decomp else [])
    os.makedirs(os.path.join(where, "build"))
    pre, env_desc = pre_fn(where)
    remake_in(where, pre, logf)
    ok, text = sha256sum_check(where, pre, logf)
    out = decomp_outputs(where)
    shutil.rmtree(where, ignore_errors=True)
    return out, text, env_desc


def carry_psyq(decomp, game, main):
    """Copy the PSY-Q SDK files the decomp ignores (psyq/include,
    psyq/lib) into the worktree's decomp, from OPENPETE_PSYQ_DIR or the
    main checkout's decomp psyq/. Existing files are kept. Returns the
    number of files copied."""
    src = os.environ.get("OPENPETE_PSYQ_DIR") or os.path.join(
        main, "games", game, game, "psyq")
    dst = os.path.join(decomp, "psyq")
    n = 0
    if not os.path.isdir(src):
        return 0
    for dp, _, fns in os.walk(src):
        for f in fns:
            s = os.path.join(dp, f)
            d = os.path.join(dst, os.path.relpath(s, src))
            if os.path.lexists(d):
                continue
            os.makedirs(os.path.dirname(d), exist_ok=True)
            shutil.copyfile(s, d)
            n += 1
    return n


def nested_submodules(decomp, main_decomp, logf, from_archive):
    """Give each submodule of the decomp (tools/maspsx) a worktree of the
    main checkout's nested module repository at the decomp's pin; under
    --from-archive, `git submodule update --init` from the remote."""
    out = pb.git("config", "-f", ".gitmodules", "--get-regexp",
                 r"^submodule\..*\.path$", cwd=decomp, check=False) or ""
    made = []
    for line in out.splitlines():
        path = line.split()[1]
        ap = os.path.join(decomp, path)
        if from_archive:
            run(["git", "-C", decomp, "submodule", "update", "--init", "--",
                 path], logf=logf, what="decomp submodule %s" % path)
            continue
        pin = (pb.git("ls-files", "-s", "--", path, cwd=decomp) or "").split()
        if len(pin) < 2:
            raise Refusal("decomp submodule %s has no gitlink" % path)
        moddir = pb.git("rev-parse", "--absolute-git-dir",
                        cwd=os.path.join(main_decomp, path), check=False)
        if not moddir:
            raise Refusal("the main checkout has no repository at %s"
                          % os.path.join(main_decomp, path))
        if os.path.isdir(ap) and not os.listdir(ap):
            os.rmdir(ap)
        run(["git", "-C", moddir.strip(), "worktree", "add", "-q",
             "--detach", ap, pin[1]], logf=logf,
            what="worktree for decomp submodule %s" % path)
        made.append((moddir.strip(), ap))
    return made


def build_psxrecomp(wt, logf):
    """Configure and build PSXRecomp from the worktree's sources into the
    worktree's own build directory."""
    src = os.path.join(wt, "third_party/psxrecomp/recompiler")
    out = os.path.join(wt, "third_party/psxrecomp/build")
    run(["cmake", "-S", src, "-B", out, "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release"], logf=logf, what="psxrecomp configure")
    run(["cmake", "--build", out, "-j"], logf=logf, what="psxrecomp build")


@contextlib.contextmanager
def rewriter_decomp(wt, game):
    """Point the AST rewriter at the worktree's own decomp for the span of
    regen-manifest.sh. tools/rewrite/parser.py takes the decomp's include
    directories from `spyro-1` beside the checkout's parent directory,
    which for a worktree under worktrees is worktrees/
    spyro-1: that path is made a link to the worktree's pinned decomp under
    an exclusive lock and removed afterwards. Only a game carrying
    manifest/native.toml runs the rewriter, and the parser names spyro-1."""
    if game != "spyro-1" or not os.path.isfile(
            os.path.join(wt, "games", game, "manifest", "native.toml")):
        yield
        return
    import fcntl
    parent = os.path.dirname(wt)
    dst = os.path.join(parent, "spyro-1")
    lock = open(os.path.join(parent, ".rewriter-decomp.lock"), "w")
    fcntl.flock(lock, fcntl.LOCK_EX)
    try:
        if os.path.lexists(dst):
            raise Refusal("%s exists; the rewriter would read that decomp "
                          "instead of the worktree's" % dst)
        os.symlink(os.path.join(wt, "games", game, game), dst)
        try:
            yield
        finally:
            os.unlink(dst)
    finally:
        fcntl.flock(lock, fcntl.LOCK_UN)
        lock.close()


def regens(wt, game, logf):
    """regen-manifest.sh, then regen-recomp.sh, in the worktree."""
    env = dict(os.environ, GAME=game)
    with rewriter_decomp(wt, game):
        run(["bash", "scripts/regen-manifest.sh"], cwd=wt, env=env,
            logf=logf, what="regen-manifest.sh")
    run(["bash", "scripts/regen-recomp.sh"], cwd=wt, env=env, logf=logf,
        what="regen-recomp.sh")


def identity(wt, game):
    """(recomp input digest, generated-tree digest) of the worktree."""
    saved = pb.repo_root
    pb.repo_root = lambda: wt
    try:
        return pb.recomp_input_digest(game), pb.generated_tree_digest(game)
    finally:
        pb.repo_root = saved


def compare_to_main(wt, game, main):
    """The regens' outputs (globals.toml, symbols.toml) against the main
    checkout's copies, byte for byte. Returns the differing paths."""
    diff = []
    for rel in ("games/%s/manifest/globals.toml" % game,
                "games/%s/manifest/symbols.toml" % game):
        a, b = os.path.join(main, rel), os.path.join(wt, rel)
        ha = pb.sha256_file(a) if os.path.isfile(a) else "absent"
        hb = pb.sha256_file(b) if os.path.isfile(b) else "absent"
        if ha != hb:
            diff.append(os.path.basename(rel))
    return diff


# ---- engine build and capture ------------------------------------------------

def container(wt, cmd, logf, what, extra_env=None):
    """Run a bash command line in the capture image with the worktree as
    the mounted tree."""
    env = dict(os.environ, OPENPETE_CAPTURE_TREE=wt)
    env.update(extra_env or {})
    run(["bash", os.path.join(ROOT, "scripts/golden-capture-container.sh"),
         "run", "--", "bash", "-euo", "pipefail", "-c", cmd], env=env,
        logf=logf, what=what)


def engine_build(wt, game, logf):
    """Fresh configure and build of the game target in the capture image.

    OPENPETE_GAMES names the one game: the worktree holds only its
    recompiled sources, and a game CMakeLists without them does not
    configure. OPENPETE_BUILD_TESTS is off: the ctest registrations name
    every game's binary and fail to generate without them, and no test
    target enters the game binary."""
    target = "openpete-" + game.replace("-", "")
    cmd = ("rm -rf build/linux-release-container && "
           "cmake -S . -B build/linux-release-container -G Ninja "
           "-DCMAKE_PROJECT_openpete_INCLUDE=$PWD/cmake/presets/"
           "linux-release.cmake -DCMAKE_C_COMPILER=/usr/bin/gcc-11 "
           "-DCMAKE_CXX_COMPILER=/usr/bin/g++-11 -DOPENPETE_GAMES=" + game +
           " -DOPENPETE_BUILD_TESTS=OFF && "
           "cmake --build build/linux-release-container --target " + target)
    container(wt, cmd, logf, "in-container configure and build")


def corpus_rows(wt, game, tier):
    """{tile prefix: (kind, row name)} over the worktree's corpus
    manifests: a bk2 row keys by its name, a freecam row by its pose slug
    (the name plus the first six hex of the pose's md5, as
    freecam-corpus.sh forms it)."""
    script = ('source scripts/presentation-game.sh >/dev/null 2>&1; '
              'PRESENTATION_TIER="$2" pg_resolve "$1" >/dev/null 2>&1; '
              'printf "%s\\n%s\\n" "$PG_CORPUS_MANIFEST" '
              '"$PG_FREECAM_MANIFEST"')
    p = subprocess.run(["bash", "-c", script, "_", game, tier], cwd=wt,
                       capture_output=True, text=True,
                       env=dict(os.environ, PRESENTATION_TIER=tier))
    lines = (p.stdout.split("\n") + ["", ""])[:2]
    out = {}
    if lines[0] and os.path.isfile(os.path.join(wt, lines[0])):
        with open(os.path.join(wt, lines[0])) as fh:
            for ln in fh:
                name = re.sub(r"\s", "", ln.split("|", 1)[0])
                if name and not name.startswith("#"):
                    out[name] = ("bk2", name)
    if lines[1] and os.path.isfile(os.path.join(wt, lines[1])):
        with open(os.path.join(wt, lines[1])) as fh:
            for ln in fh:
                f = ln.rstrip("\n").split("|")
                name = re.sub(r"\s", "", f[0])
                if not name or name.startswith("#") or len(f) < 2:
                    continue
                pose = f[1].strip()
                if not pose:
                    continue
                h = hashlib.md5(pose.encode()).hexdigest()[:6]
                out["%s-%s" % (name, h)] = ("freecam", name)
    return out


def capture(wt, game, tier, rows, out, logf, timer):
    """Capture the corpus rows the tiles belong to, one harness run per
    row, in the capture image."""
    table = corpus_rows(wt, game, tier)
    plan = {}
    for r in rows:
        pre = pb.corpus_row(r["tile"])
        if pre not in table:
            raise Refusal("tile %s names corpus row %s, which the corpus "
                          "manifests at the revision do not hold"
                          % (r["tile"], pre))
        lod = "extended" if "__lodext__" in r["tile"] else "original"
        plan.setdefault(table[pre], set()).add(lod)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    for (kind, name), lods in sorted(plan.items()):
        script = ("tests/harness/presentation-corpus.sh" if kind == "bk2"
                  else "tests/harness/freecam-corpus.sh")
        env = {"PRESENTATION_TIER": tier,
               "OPENPETE_CAP_LODS": " ".join(sorted(lods))}
        with timer.step("capture:%s" % name):
            container(wt, "%s default %s %s --build-dir=build/linux-release-"
                      "container --game=%s" % (script, out, name, game),
                      logf, "%s %s" % (script, name), env)


def compare(rows, out, rev):
    """Digest each captured tile against its row; file the equal ones.
    Returns [(tile, verdict, want, got)]."""
    res = []
    for r in rows:
        p = os.path.join(out, r["tile"])
        if not os.path.isfile(p):
            res.append((r["tile"], "not-reproduced", r["digest"], "missing"))
            continue
        got = pb.digest_file(p)
        g = got[0] if got else "undecodable"
        if g == r["digest"]:
            wrote = pb.store_put(p, g)
            res.append((r["tile"], "reproduced" + (" filed" if wrote
                                                   else " present"),
                        r["digest"], g))
        else:
            res.append((r["tile"], "not-reproduced", r["digest"], g))
    for tile, verdict, want, got in res:
        log("%s\t%s\trevision %s\trow %s\tcaptured %s"
            % (verdict, tile, rev[:12], want, got))
    return res


# ---- state file --------------------------------------------------------------

def write_state(game, line):
    """Write decomp-reproducibility-<game> and the aggregate key into
    golden-ref.state beside the store."""
    path = pb.state_path()
    lines = []
    try:
        with open(path) as fh:
            lines = fh.read().splitlines()
    except OSError:
        pass
    key = "decomp-reproducibility-%s" % game
    lines = [ln for ln in lines
             if not ln.startswith(key + "=")
             and not ln.startswith("decomp-reproducibility=")]
    lines.append("%s=%s" % (key, line))
    per = {}
    for ln in lines:
        m = re.match(r"^decomp-reproducibility-(\S+?)=(\S+)", ln)
        if m:
            per[m.group(1)] = m.group(2)
    ok = all(per.get(g) == "pass" for g in pb.GAMES)
    agg = "%s %s" % ("pass" if ok else "fail",
                     " ".join("%s:%s" % (g, per.get(g, "unmeasured"))
                              for g in pb.GAMES))
    lines.append("decomp-reproducibility=" + agg)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + ".tmp", "w") as fh:
        fh.write("\n".join(lines) + "\n")
    os.replace(path + ".tmp", path)
    log("state %s: %s=%s" % (path, key, line))
    log("state %s: decomp-reproducibility=%s" % (path, agg))


# ---- driver ------------------------------------------------------------------

def materialize(a, rev, m, rows, timer, logf):
    """One blessed revision's rows. Returns the compare results, or None
    when --stop-after ended the run."""
    game = a.game
    main = main_checkout()
    wt, name = None, "mat-%s-%d" % (rev[:10], os.getpid())
    measure, nested = {}, []
    try:
        disc = None
        if not a.measure:
            with timer.step("disc"):
                disc = disc_check(game, rev, m, main, a.from_archive)
            if a.stop_after == "disc":
                return None
        with timer.step("worktree"):
            wt = make_worktree(rev, name, a.from_archive, logf)
            allp, mine = input_paths(game)
            refuse_links(wt, allp)
        log("worktree %s at %s" % (wt, rev[:12]))
        if a.stop_after == "worktree":
            return None
        decomp = os.path.join(wt, "games", game, game)
        main_decomp = os.path.join(main, "games", game, game)
        os.makedirs(os.path.join(decomp, "build"), exist_ok=True)
        with timer.step("decomp-inputs"):
            nested += nested_submodules(decomp, main_decomp, logf,
                                        a.from_archive)
            n = carry_psyq(decomp, game, main)
        log("decomp inputs: %d PSY-Q SDK file(s) carried in, %d nested "
            "submodule worktree(s)" % (n, len(nested)))
        if disc:
            with timer.step("disc-carry"):
                carry_disc(disc, decomp)
        remake_decomp(decomp, timer, logf, measure)
        if a.measure:
            measure_decomp(decomp, main_decomp, a.log_dir, logf, timer,
                           measure)
        if a.stop_after == "decomp":
            return None
        with timer.step("psxrecomp"):
            build_psxrecomp(wt, logf)
        if a.stop_after == "psxrecomp":
            return None
        refuse_links(wt, allp)
        with timer.step("regen"):
            regens(wt, game, logf)
        for rel in mine:
            if not os.path.lexists(os.path.join(wt, rel)) and \
                    not rel.startswith("games/%s/%s/build" % (game, game)):
                raise Refusal("%s was not regenerated in the worktree" % rel)
        refuse_links(wt, allp)
        if a.measure:
            with timer.step("measure-compare"):
                measure["compare"] = compare_to_main(wt, game, main)
            log("regens against the main checkout: %s"
                % (", ".join(measure["compare"]) + " differ"
                   if measure["compare"] else "identical"))
            with timer.step("disc"):
                disc = disc_check(game, rev, m, main, a.from_archive)
            with timer.step("disc-carry"):
                carry_disc(disc, decomp)
        if a.stop_after == "regen":
            return None
        with timer.step("identity"):
            rin, gen = identity(wt, game)
            want = {(r["recomp_input"], r["generated_tree"]) for r in rows}
            measure["identity"] = (rin, gen)
            if (rin, gen) not in want:
                wr, wg = sorted(want)[0]
                raise Refusal("recomp identity mismatch at %s: input %s "
                              "(row %s), generated tree %s (row %s)"
                              % (rev[:12], rin, wr, gen, wg))
        log("recomp identity matches the row: input %s generated %s"
            % (rin[:12], gen[:12]))
        if a.stop_after == "identity":
            return None
        with timer.step("build"):
            engine_build(wt, game, logf)
        if a.stop_after == "build":
            return None
        out = os.path.join(wt, "build", "materialize-candidate")
        capture(wt, game, a.tier, rows, out, logf, timer)
        with timer.step("compare"):
            res = compare(rows, out, rev)
        if a.log_dir:
            for r in res:
                p = os.path.join(out, r[0])
                if os.path.isfile(p):
                    shutil.copyfile(p, os.path.join(a.log_dir, r[0]))
        return res
    finally:
        if a.measure:
            measure_record(game, rev, measure)
        if wt and not a.keep_worktree:
            with timer.step("teardown"):
                for moddir, ap in nested:
                    subprocess.run(["git", "-C", moddir, "worktree", "remove",
                                    "--force", ap], capture_output=True)
                remove_worktree(wt, name, a.from_archive, logf)
        elif wt:
            log("worktree kept: %s" % wt)


def carry_disc(disc, decomp):
    """Hard-link (else copy) the checked disc files into the worktree's
    decomp build directory."""
    for src, base in disc:
        dst = os.path.join(decomp, "build", base)
        if os.path.lexists(dst):
            os.unlink(dst)
        try:
            os.link(src, dst)
        except OSError:
            shutil.copyfile(src, dst)


MEASURE_RESULT = {}


def measure_decomp(decomp, main_decomp, log_dir, logf, timer, meas):
    """The decomp-reproducibility measurement: a second re-make of a copy
    in the same environment (determinism), the re-make's outputs against
    the main checkout's copies, and, when those differ, the same re-make
    on the host (the other environment), each compared the same way."""
    first = meas["outputs"]
    main_out = decomp_outputs(main_decomp)
    with timer.step("measure-remake-2"):
        second, text2, env2 = remake_copy(
            decomp, os.path.join(log_dir, "remake-2"), container_prefix,
            logf)
    meas["deterministic"] = sorted(k for k in first
                                   if first[k] != second.get(k))
    meas["vs_main"] = sorted(k for k in first if first[k] != main_out.get(k))
    log("re-make determinism in %s: %s; against the main checkout: %s"
        % (meas["env"], "identical" if not meas["deterministic"] else
           "differs at " + ",".join(meas["deterministic"]),
           "identical" if not meas["vs_main"] else
           "differs at " + ",".join(meas["vs_main"])))
    if meas["vs_main"] and meas["env"] != "host":
        with timer.step("measure-remake-host"):
            host, texth, _ = remake_copy(
                decomp, os.path.join(log_dir, "remake-host"),
                lambda d: ([], "host"), logf)
        meas["host_sha256sum"] = texth
        meas["host_vs_main"] = sorted(k for k in host
                                      if host[k] != main_out.get(k))
        log("host re-make: sha256sum -c %s; against the main checkout: %s"
            % (texth, "identical" if not meas["host_vs_main"] else
               "differs at " + ",".join(meas["host_vs_main"])))


def measure_record(game, rev, meas):
    """Hold the measurement parts until the compare result is known."""
    MEASURE_RESULT.update(game=game, rev=rev, **meas)


def measure_write(game, rev, res, refusal):
    """Write the reproducibility line for one game: pass when the re-make
    passes its checksum check, is deterministic, matches the main
    checkout's outputs and regens, and every materialized row reproduces."""
    r = MEASURE_RESULT
    diff = r.get("compare")
    pin = pb.git("rev-parse", "%s:games/%s/%s" % (rev, game, game),
                 check=False) or "unknown"
    det = r.get("deterministic")
    vs = r.get("vs_main")
    remake_ok = (r.get("sha256sum", "").startswith("ok") and det == []
                 and vs == [] and diff == [])
    rows_ok = bool(res) and all(x[1].startswith("reproduced") for x in res)
    ok = remake_ok and rows_ok and not refusal

    def lst(v):
        return ("not-reached" if v is None else
                "identical" if not v else "differs:" + ",".join(v))
    parts = ["pass" if ok else "fail",
             "submodule=%s" % pin.strip()[:12],
             "env=%s" % r.get("env", "not-reached"),
             "sha256sum=%s" % r.get("sha256sum", "not-reached"),
             "deterministic=%s" % lst(det),
             "remake-vs-main=%s" % lst(vs)]
    if "host_vs_main" in r:
        parts += ["host-sha256sum=%s" % r["host_sha256sum"],
                  "host-remake-vs-main=%s" % lst(r["host_vs_main"])]
    parts += ["regens-vs-main=%s" % lst(diff),
              "materialize=%s" % ("%d/%d-reproduced" % (
                  sum(1 for x in res if x[1].startswith("reproduced")),
                  len(res)) if res else "not-reached"),
              "at=%s" % rev[:12], "date=%s" % time.strftime("%Y-%m-%d")]
    if refusal:
        parts.append("refused=%s" % re.sub(r"\s+", "_", refusal)[:200])
    write_state(game, " ".join(parts))


def main(argv=None):
    """Parse the command line, resolve the rows, materialize per revision."""
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tiles", nargs="+")
    ap.add_argument("--game", choices=pb.GAMES, default="spyro-1")
    ap.add_argument("--tier", choices=sorted(pb.TIERS),
                    default="presentation")
    ap.add_argument("--at", help="read the rows as committed at this "
                    "revision (default: the working tree's manifest)")
    ap.add_argument("--from-archive", action="store_true")
    ap.add_argument("--measure", action="store_true")
    ap.add_argument("--keep-worktree", action="store_true")
    ap.add_argument("--stop-after", choices=STEPS)
    ap.add_argument("--log-dir")
    a = ap.parse_args(argv)
    for mk in CTEST_MARKERS:
        if os.environ.get(mk) is not None:
            print("[materialize] refused: %s is set; a materialize is a manual "
                  "adjudication command and never runs under ctest (a build "
                  "plus a recapture exceeds the presentation row's timeout)"
                  % mk, file=sys.stderr)
            return 1
    if a.game not in pb.TIERS[a.tier]:
        ap.error("tier %s has no game %s" % (a.tier, a.game))
    a.log_dir = os.path.abspath(a.log_dir or os.path.join(
        ROOT, "build", "golden-materialize",
        time.strftime("%Y%m%d-%H%M%S") + "-" + a.game))
    os.makedirs(a.log_dir, exist_ok=True)
    logf = os.path.join(a.log_dir, "materialize.log")
    timer = Timer(a.log_dir)
    rc = 0
    results = []
    try:
        with timer.step("rows"):
            m, rows = load_rows(a.game, a.tier, a.at, a.tiles)
        groups = {}
        for r in rows:
            groups.setdefault(r["revision"], []).append(r)
        for rev, grp in sorted(groups.items()):
            if any(r["origin"] == "archive-only" for r in grp):
                a.from_archive = True
            if not a.from_archive and not pb.resolve(ROOT, rev):
                raise Refusal("revision %s does not resolve in this "
                              "repository (an archive-only row takes "
                              "--from-archive)" % rev)
            refusal = None
            res = []
            try:
                res = materialize(a, rev, m, grp, timer, logf) or []
            except Refusal as e:
                refusal = str(e)
                raise
            finally:
                if a.measure:
                    measure_write(a.game, rev, res, refusal)
            results += res
    except Refusal as e:
        log("REFUSED: %s" % e)
        rc = 1
    total = sum(dt for _, dt, _ in timer.rows)
    log("timings (%s):" % os.path.join(a.log_dir, "timings.tsv"))
    for n, dt, st in timer.rows:
        log("  %-28s %8.1f s  %s" % (n, dt, st))
    log("  %-28s %8.1f s" % ("total", total))
    bad = [r for r in results if not r[1].startswith("reproduced")]
    if results:
        log("rows: %d reproduced, %d not reproduced"
            % (len(results) - len(bad), len(bad)))
    return 1 if (rc or bad) else 0


if __name__ == "__main__":
    sys.exit(main())
