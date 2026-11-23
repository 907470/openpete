#!/usr/bin/env python3
"""The build-directory lock: a build holds it exclusive for its whole run, a
gate holds it shared while it reads the build's binary.

The lock is flock(2) on <build dir>/.openpete-build.lock, where the build
directory is the nearest directory at or above a path that holds
CMakeCache.txt. Gates share it, so two gates may read one build; a build
excludes every gate, so no gate starts on a binary a build is replacing and
no build replaces a binary a gate is reading. Each side takes the lock
before it acts, so neither side's check can be overtaken by the other.

Build side (`build-hold --dir D`): the CMake target openpete_build_lock
(cmake/openpete_build_lock.cmake), on which every other target of the build
depends, runs it first in every Ninja build. It finds the build driver (the
nearest `ninja` ancestor) and starts a holder of LOCK_EX that serves it, so
the lock lasts until the build ends, however it ends. A driver by another
name (an IDE's build runner) is the nearest ancestor that is not a shell. While gates hold the
lock it waits, naming them. A build under a gate of its own process tree
that reads the same directory would wait forever and fails instead (exit 3).

Gate side (`read --for-pid P --path BIN`, or read() in-process): waits
while a build holds the lock, naming it, then holds LOCK_SH until P (or
this process) exits. A gate whose own process tree holds the lock (a
nested gate, or a gate a build runs as one of its targets) proceeds without
waiting. A path outside any build directory (a copied exe) takes no lock.

OPENPETE_ALLOW_BUILD_DURING_GATE=1 lets a build proceed without the lock,
for a gate that is stale or abandoned.

Exit codes: 0 held (or no lock applies), 3 a build under its own gate, 64
usage, 75 timed out waiting.
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flock_hold                                           # noqa: E402

LOCK_NAME = ".openpete-build.lock"
DRIVERS = ("ninja", "ninja-build", "samu")
WRAPPERS = re.compile(r"(?:ba|da|z|k)?sh|python[\d.]*")
REPORT_EVERY_S = 30.0
POLL_S = 0.5
EXIT_NESTED = 3
EXIT_TIMEOUT = 75
BYPASS_ENV = "OPENPETE_ALLOW_BUILD_DURING_GATE"

_HOLDS = {}         # lock path -> this process's in-process flock_hold.Hold


def build_dir_of(path):
    """The nearest directory at or above `path` holding CMakeCache.txt, or
    None."""
    p = os.path.realpath(path)
    if not os.path.isdir(p):
        p = os.path.dirname(p)
    while True:
        if os.path.isfile(os.path.join(p, "CMakeCache.txt")):
            return p
        parent = os.path.dirname(p)
        if parent == p:
            return None
        p = parent


def lock_path(build_dir):
    """The lock file of a build directory."""
    return os.path.join(build_dir, LOCK_NAME)


def _comm(pid):
    try:
        with open("/proc/%d/comm" % pid) as f:
            return f.read().strip()
    except OSError:
        return ""


def _describe(path, mode):
    """The processes holding `path` in `mode`: each holder named by the
    process it serves."""
    out = []
    for p in sorted(set(flock_hold.lockers(path, mode))):
        s = flock_hold.served_pid(p, path) or p
        try:
            with open("/proc/%d/cmdline" % s, "rb") as f:
                cmd = f.read().replace(b"\0", b" ").decode(errors="replace").strip()
        except OSError:
            cmd = "?"
        out.append("pid %d: %s" % (s, cmd[:160]))
    return out


def _report(what, path, mode, since, last):
    now = time.monotonic()
    if last[0] == 0 or now - last[0] >= REPORT_EVERY_S:
        sys.stderr.write("build-lock: %s waits (%.0f s) on %s, held by\n%s\n" % (
            what, now - since, os.path.dirname(path),
            "\n".join("  " + d for d in _describe(path, mode)) or "  (unknown)"))
        last[0] = now


def read(path, label="gate", for_pid=None, timeout=None):
    """Hold the build directory of `path` shared for the caller's run: True,
    or False at the timeout. True at once when `path` lies outside a build
    directory, when the caller already holds it, or when the caller's own
    process tree holds it in either mode."""
    bdir = build_dir_of(path)
    if bdir is None:
        return True
    lock = lock_path(bdir)
    me = for_pid or os.getpid()
    if (lock in _HOLDS and not for_pid) or (for_pid and flock_hold.holders_for(lock, me)):
        return True
    if flock_hold.covered(lock, flock_hold.lineage(me)) is not None:
        return True
    since, last = time.monotonic(), [0.0]
    while True:
        if for_pid:
            if flock_hold.spawn(lock, flock_hold.SH, for_pid, label):
                return True
        else:
            h = flock_hold.hold(lock, flock_hold.SH)
            if h is not None:
                _HOLDS[lock] = h
                return True
        if timeout is not None and time.monotonic() - since >= timeout:
            sys.stderr.write("build-lock: %s gave up after %.0f s: a build holds %s\n"
                             % (label, timeout, bdir))
            return False
        _report(label, lock, "WRITE", since, last)
        time.sleep(POLL_S)


def release(path):
    """Release this process's in-process hold on the build directory of
    `path`, if any."""
    bdir = build_dir_of(path)
    h = _HOLDS.pop(lock_path(bdir), None) if bdir else None
    if h is not None:
        h.release()


def driver_pid():
    """The build driver this command runs under: the nearest ancestor named
    in DRIVERS, else the nearest ancestor that is not a shell or Python (the
    `sh -c` a generator wraps the command in exits with this step)."""
    anc = flock_hold.lineage()[1:]
    for p in anc:
        if _comm(p) in DRIVERS:
            return p
    for p in anc:
        if not WRAPPERS.match(_comm(p)):
            return p
    return os.getppid()


def build_hold(build_dir):
    """Hold `build_dir` exclusive until the build driver exits. Returns the
    exit status."""
    if os.environ.get(BYPASS_ENV):
        sys.stderr.write("build-lock: %s set; building without the lock\n" % BYPASS_ENV)
        return 0
    lock = lock_path(os.path.realpath(build_dir))
    drv = driver_pid()
    if flock_hold.holders_for(lock, drv):
        return 0
    since, last = time.monotonic(), [0.0]
    while True:
        if flock_hold.spawn(lock, flock_hold.EX, drv, "build"):
            return 0
        own = flock_hold.covered(lock, flock_hold.lineage(drv), "READ")
        if own is not None:
            sys.stderr.write(
                "build-lock: REFUSED: process %d, which runs this build, is a gate reading "
                "%s; it would read a binary this build replaces. Build before the gate "
                "starts.\n" % (own, build_dir))
            return EXIT_NESTED
        _report("build", lock, "READ", since, last)
        time.sleep(POLL_S)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("read")
    p.add_argument("--path", required=True, help="the binary or build directory the gate reads")
    p.add_argument("--for-pid", type=int, required=True)
    p.add_argument("--label", default="gate")
    p.add_argument("--timeout", type=float, default=None)
    p = sub.add_parser("build-hold")
    p.add_argument("--dir", required=True)
    a = ap.parse_args(argv)
    if a.cmd == "read":
        return 0 if read(a.path, a.label, a.for_pid, a.timeout) else EXIT_TIMEOUT
    return build_hold(a.dir)


if __name__ == "__main__":
    sys.exit(main())
