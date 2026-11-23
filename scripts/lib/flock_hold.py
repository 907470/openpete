#!/usr/bin/env python3
"""Reader/writer holds on a flock(2) lock file that outlive a check.

A gate that tests a lock and returns leaves a gap between the test and the
caller's launch in which the other side can take the lock. A hold closes the
gap: the lock is taken before the caller proceeds and released when the
caller exits. Two forms:

  in-process   hold(path, mode) keeps a non-inheritable descriptor locked in
               this Python process until release() or exit.
  served       spawn(path, mode, for_pid) starts a detached holder process
               (`flock_hold.py _serve`) that takes the lock on its own
               descriptor, reports on a pipe, and keeps it until `for_pid`
               exits or it receives SIGTERM. A shell uses this form: a
               descriptor a shell holds is inherited by every child, and a
               child that daemonizes (podman's session dbus-daemon, an ssh
               control master) would keep the lock after the shell exits.

A lock is attributed to a process tree through /proc/locks: each granted
flock names the pid that took it, and a holder's command line names the
pid it serves (`--for-pid`). covered() answers whether one of a set of pids
holds the lock, directly or through a holder; a process under such a hold
proceeds without taking its own, since waiting on the other side would wait
on its own ancestor.
"""
import argparse
import errno
import fcntl
import os
import select
import signal
import subprocess
import sys
import time

SH, EX = fcntl.LOCK_SH, fcntl.LOCK_EX
SERVE = "_serve"


def try_lock(fd, mode):
    """Take `mode` on `fd` without blocking; True when taken."""
    try:
        fcntl.flock(fd, mode | fcntl.LOCK_NB)
        return True
    except OSError as e:
        if e.errno in (errno.EWOULDBLOCK, errno.EAGAIN):
            return False
        raise


def open_lock(path):
    """A non-inheritable read-write descriptor on `path`, created."""
    return os.open(path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o644)


def ppid(pid):
    """The parent pid of `pid`, 0 when it cannot be read."""
    try:
        with open("/proc/%d/stat" % pid) as f:
            return int(f.read().rsplit(")", 1)[1].split()[1])
    except (OSError, ValueError, IndexError):
        return 0


def lineage(pid=None):
    """`pid` (default this process) and its ancestors, nearest first, without
    pid 1."""
    out, p = [], os.getpid() if pid is None else pid
    while p > 1 and p not in out:
        out.append(p)
        p = ppid(p)
    return out


def _dev_key(path):
    st = os.stat(path)
    return "%02x:%02x:%d" % (os.major(st.st_dev), os.minor(st.st_dev), st.st_ino)


def lockers(path, mode=None):
    """Pids holding a granted flock on `path` (/proc/locks; waiters `->` are
    skipped); `mode` "READ" or "WRITE" filters."""
    try:
        key = _dev_key(path)
        with open("/proc/locks") as f:
            lines = f.read().splitlines()
    except OSError:
        return []
    out = []
    for line in lines:
        w = line.split()
        if len(w) < 6 or w[1] != "FLOCK" or w[5] != key:
            continue
        if mode and w[3] != mode:
            continue
        try:
            out.append(int(w[4]))
        except ValueError:
            pass
    return out


def served_pid(pid, path=None):
    """The pid a holder process serves (its `--for-pid`), or None when `pid`
    is not a holder (of `path`, when given)."""
    try:
        with open("/proc/%d/cmdline" % pid, "rb") as f:
            argv = [a.decode(errors="replace") for a in f.read().split(b"\0")]
    except OSError:
        return None
    if SERVE not in argv or "--for-pid" not in argv:
        return None
    if path is not None and ("--lock" not in argv or
                             os.path.realpath(argv[argv.index("--lock") + 1]) != os.path.realpath(path)):
        return None
    try:
        return int(argv[argv.index("--for-pid") + 1])
    except (IndexError, ValueError):
        return None


def covered(path, pids, mode=None):
    """The pid in `pids` that holds `path` (`mode` "READ"/"WRITE" or any),
    directly or through a holder it is served by; None when none does."""
    pids = set(pids)
    for p in lockers(path, mode):
        if p in pids:
            return p
        s = served_pid(p, path)
        if s in pids:
            return s
    return None


def holders_for(path, pid):
    """Holder processes of `path` that serve `pid`."""
    return [p for p in set(lockers(path)) if served_pid(p, path) == pid]


class Hold:
    """An in-process hold: a locked non-inheritable descriptor."""

    def __init__(self, fd, path, mode):
        self.fd, self.path, self.mode = fd, path, mode

    def release(self):
        """Unlock and close the descriptor (idempotent)."""
        if self.fd is not None:
            fcntl.flock(self.fd, fcntl.LOCK_UN)
            os.close(self.fd)
            self.fd = None


def hold(path, mode):
    """Take `mode` on `path` without blocking; a Hold, or None when the
    other side holds it."""
    fd = open_lock(path)
    if try_lock(fd, mode):
        return Hold(fd, path, mode)
    os.close(fd)
    return None


def spawn(path, mode, for_pid, label=""):
    """Start a detached holder of `mode` on `path` serving `for_pid`; its
    pid once it holds the lock, None when the lock was not free."""
    r, w = os.pipe()
    argv = [sys.executable, os.path.abspath(__file__), SERVE, "--lock", path,
            "--mode", "ex" if mode == EX else "sh", "--for-pid", str(for_pid),
            "--ready-fd", str(w), "--label", label or "-"]
    p = subprocess.Popen(argv, pass_fds=(w,), start_new_session=True,
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    os.close(w)
    with os.fdopen(r) as f:
        line = f.read().strip()
    if line == "ok":
        return p.pid
    p.wait()
    return None


def stop(pid):
    """Stop a holder this process tree started and wait until its lock is
    gone (the holder releases on SIGTERM)."""
    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    for _ in range(200):
        if not os.path.isdir("/proc/%d" % pid):
            return
        try:
            with open("/proc/%d/stat" % pid) as f:
                if f.read().rsplit(")", 1)[1].split()[0] == "Z":
                    return
        except OSError:
            return
        time.sleep(0.01)


def _serve(a):
    """The holder: take the lock, report, hold until the served pid exits or
    SIGTERM arrives."""
    fd = open_lock(a.lock)
    out = os.fdopen(a.ready_fd, "w")
    if not try_lock(fd, EX if a.mode == "ex" else SH):
        out.write("busy\n")
        out.close()
        return 1
    pidfd = None
    try:
        if hasattr(os, "pidfd_open"):
            pidfd = os.pidfd_open(a.for_pid)
        else:
            os.kill(a.for_pid, 0)
    except OSError:
        out.write("gone\n")     # the served process already exited
        out.close()
        return 1
    stop_r, stop_w = os.pipe()
    signal.signal(signal.SIGTERM, lambda *_: os.write(stop_w, b"x"))
    out.write("ok\n")
    out.close()
    while True:
        try:
            # Without pidfd (Python < 3.9) the served pid is polled.
            ready = select.select([stop_r] + ([pidfd] if pidfd is not None else []), [], [],
                                  None if pidfd is not None else 0.5)[0]
        except InterruptedError:
            continue
        if ready or not os.path.isdir("/proc/%d" % a.for_pid):
            break
    fcntl.flock(fd, fcntl.LOCK_UN)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser(SERVE)
    p.add_argument("--lock", required=True)
    p.add_argument("--mode", choices=("sh", "ex"), required=True)
    p.add_argument("--for-pid", type=int, required=True)
    p.add_argument("--ready-fd", type=int, required=True)
    p.add_argument("--label", default="-")
    a = ap.parse_args(argv)
    return _serve(a)


if __name__ == "__main__":
    sys.exit(main())
