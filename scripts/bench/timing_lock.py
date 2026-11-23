#!/usr/bin/env python3
"""The timing window: one exclusive lock that a timing job holds while its
legs run, and that every other launch on the machine waits on.

The lock is flock(2) LOCK_EX on <bench>/timing.lock (<bench> is
$OPENPETE_BENCH_DIR, else ~/.local/share/openpete/bench). flock is atomic
between contenders and the kernel drops it when the holding process exits
for any reason, so a dead owner never leaves a held lock behind; the file's
content (JSON: token, pid, purpose, owner, host, start, ttl_s) names the
current holder and is meaningful only while the lock is held. A fleet host
has its own lock, timing-<host>.lock, so a ptah leg and a local leg do not
exclude each other.

The local lock's holder also writes <bench>/TIMING-WINDOW, one line
`<purpose> <start> lock-token=<token>`, and removes it on release. Scripts
that test that path keep working. A TIMING-WINDOW line without
`lock-token=` was written by hand (the older convention) and counts as a
held window; one carrying a token whose lock is free is stale (its holder
was killed) and is removed by the next lock operation.

The holder exports OPENPETE_TIMING_TOKEN to its job; a process whose
environment carries the current holder's token is inside the window and
passes the gate.

Launches hold the window shut. gate() (the library form; `gate --for-pid`
for a shell; scripts/lib/timing_gate.sh) waits while the window is held or
wanted and then takes LOCK_SH on the same file, held until the caller exits:
a timing job's LOCK_EX cannot be granted between a launch's check and its
start. A timing job first takes LOCK_EX on <bench>/timing.want (the want
lock) and keeps it until it releases the window; while it is held, new
launches wait, and a caller that already holds the window shut releases its
hold at its next gate() and waits there. The want lock gives a waiting
timing job precedence over new launches, and a long harness that gates each
launch yields between launches. A process covered by a hold of its own
process tree (its ancestors' direct holds or their holders) proceeds
without waiting; a container launched under a hold receives
OPENPETE_TIMING_COVERED=1 for the same purpose. A timing job that is itself
under such a hold would wait on its own ancestor and refuses instead (exit
69); its own in-process hold is dropped before it takes the window.

Commands:
  run --purpose P [--host H] [--hold-display] [--timeout S] -- CMD...
        wait for the window, hold it while CMD runs, release; exit CMD's
        status. The preferred form: the lock lives exactly as long as the
        job.
  acquire --purpose P --ttl S [--host H] [--watch-pid PID]
        start a detached holder that keeps the window for at most S seconds
        (or until PID exits); prints `OPENPETE_TIMING_TOKEN=<token>`. For a
        window that spans several separate shell commands.
  release --token T [--host H]
        stop the detached holder that owns token T.
  status [--host H]      print the window's state; exit 0 free, 1 held.
  wait [--host H] [--label L] [--timeout S]
        block while another job holds or wants the window; exit 0 when free,
        75 at the timeout. Never returns 0 while the window is held by
        someone else. It holds nothing: a launch after it can start beside
        a timing job that took the window in between; launch through
        `shared` or gate() instead.
  shared [--host H] [--label L] [--timeout S] -- CMD...
        gate(), then run CMD with the window held shut; exit CMD's status.
  gate --for-pid P [--host H] [--label L] [--timeout S]
        gate() on behalf of process P (a shell): a holder process keeps the
        window shut until P exits; exit 0, or 75 at the timeout.

Exit codes: 0 ok, 1 held (status), 64 usage, 69 a timing job under a hold
of its own process tree, 75 timed out waiting.
"""
import argparse
import datetime
import errno
import fcntl
import json
import os
import signal
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib"))
import flock_hold                                           # noqa: E402

TOKEN_ENV = "OPENPETE_TIMING_TOKEN"
INDICATOR = "TIMING-WINDOW"
TAG = "lock-token="
POLL_S = 2.0
REPORT_EVERY_S = 60.0
EXIT_TIMEOUT = 75
EXIT_NESTED = 69
COVERED_ENV = "OPENPETE_TIMING_COVERED"


class TimingLockError(Exception):
    """A timing job that cannot take the window: its own process tree holds
    it shut."""


def bench_dir():
    """The directory holding the lock files and the indicator, created."""
    d = os.environ.get("OPENPETE_BENCH_DIR") or os.path.expanduser(
        "~/.local/share/openpete/bench")
    os.makedirs(d, exist_ok=True)
    return d


def lock_path(host=None):
    """The lock file of `host` (None or "local": this machine and its
    Windows side, which share the CPU)."""
    name = "timing.lock" if host in (None, "", "local", "windows") else "timing-%s.lock" % host
    return os.path.join(bench_dir(), name)


def _local(host):
    return host in (None, "", "local", "windows")


def want_path(host=None):
    """The want lock of `host`: LOCK_EX by a timing job from its first wait
    until it releases the window."""
    name = "timing.want" if _local(host) else "timing-%s.want" % host
    return os.path.join(bench_dir(), name)


def indicator_path():
    """The TIMING-WINDOW indicator path."""
    return os.path.join(bench_dir(), INDICATOR)


def _now():
    return datetime.datetime.now().astimezone().isoformat(timespec="seconds")


def _read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return None


def _probe(path):
    """(held, info) for a lock file: held when LOCK_SH cannot be taken
    without blocking; info is the holder record when held."""
    try:
        fd = os.open(path, os.O_RDONLY | os.O_CREAT | os.O_CLOEXEC, 0o644)
    except OSError:
        return False, None
    try:
        fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
    except OSError as e:
        if e.errno not in (errno.EWOULDBLOCK, errno.EAGAIN):
            raise
        os.close(fd)
        try:
            info = json.loads(_read(path) or "{}")
        except ValueError:
            info = {}
        return True, info
    fcntl.flock(fd, fcntl.LOCK_UN)
    os.close(fd)
    return False, None


def state(host=None):
    """The window's state: {"held", "lock" (holder record or None),
    "legacy" (a hand-written TIMING-WINDOW line or None), "mine"}.

    "mine" is True when this process's OPENPETE_TIMING_TOKEN is the holder's.
    A tagged indicator whose lock is free is stale and removed here."""
    held, info = _probe(lock_path(host))
    legacy = None
    if host in (None, "", "local", "windows"):
        line = _read(indicator_path())
        if line is not None:
            line = line.strip()
            if TAG in line:
                if not held:
                    _remove_indicator(line.split(TAG, 1)[1].split()[0])
            else:
                legacy = line or "(empty marker)"
    mine = bool(held and info and info.get("token")
                and os.environ.get(TOKEN_ENV) == info.get("token"))
    wanted = None
    if not held:
        wheld, winfo = _probe(want_path(host))
        if wheld and (winfo or {}).get("pid") != os.getpid():
            wanted = winfo or {}
    return {"held": held or legacy is not None or wanted is not None,
            "lock": info if held else None, "legacy": legacy, "mine": mine,
            "wanted": wanted}


def describe(st):
    """One line naming who holds the window, or `free`."""
    if not st["held"]:
        return "free"
    if st["lock"]:
        i = st["lock"]
        return "held by %s (pid %s, owner %s, host %s, since %s%s)" % (
            i.get("purpose", "?"), i.get("pid", "?"), i.get("owner", "?"),
            i.get("host", "?"), i.get("start", "?"),
            ", this job" if st["mine"] else "")
    if st["legacy"]:
        return "held by a hand-written TIMING-WINDOW marker: %s" % st["legacy"]
    if st.get("readers") is not None:
        return "free of timing jobs, held shut by %d launch(es) (pids %s)" % (
            len(st["readers"]), " ".join(map(str, st["readers"][:8])) or "?")
    w = st["wanted"]
    return "wanted by %s (pid %s, since %s), waiting for launches holding it shut" % (
        w.get("purpose", "?"), w.get("pid", "?"), w.get("start", "?"))


def _remove_indicator(token):
    """Remove TIMING-WINDOW when it carries `token` (never another's)."""
    p = indicator_path()
    line = _read(p)
    if line is not None and (TAG + token) in line:
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass


class Holder:
    """A held window: the open locked descriptor and the token."""

    def __init__(self, fd, token, host, path, want_fd=None):
        self.fd, self.token, self.host, self.path = fd, token, host, path
        self.want_fd = want_fd

    def release(self):
        """Remove the indicator (local lock), drop the lock, then the want
        lock."""
        if self.fd is None:
            return
        if self.host in (None, "", "local", "windows"):
            _remove_indicator(self.token)
        fcntl.flock(self.fd, fcntl.LOCK_UN)
        os.close(self.fd)
        self.fd = None
        _close_want(self.want_fd)
        self.want_fd = None


def _close_want(fd):
    if fd is not None:
        fcntl.flock(fd, fcntl.LOCK_UN)
        os.close(fd)


def _take_want(purpose, host):
    """LOCK_EX on the want lock without blocking, its record written; the
    descriptor, or None when another timing job wants the window."""
    fd = flock_hold.open_lock(want_path(host))
    if not flock_hold.try_lock(fd, flock_hold.EX):
        os.close(fd)
        return None
    os.ftruncate(fd, 0)
    os.pwrite(fd, (json.dumps({"pid": os.getpid(), "purpose": purpose,
                               "start": _now()}) + "\n").encode(), 0)
    return fd


def _upgrade(host):
    """Before this process takes the window: drop its own in-process shared
    hold, and refuse (TimingLockError) when an ancestor holds the window
    shut, since LOCK_EX would wait on that ancestor forever."""
    release_shared(host)
    path = lock_path(host)
    anc = flock_hold.covered(path, flock_hold.lineage()[1:], "READ")
    if anc is not None:
        raise TimingLockError(
            "timing-lock: process %d, an ancestor of this timing job, holds the window "
            "shut (a launch gate); run the timing job outside that launch" % anc)
    if os.environ.get(COVERED_ENV):
        raise TimingLockError(
            "timing-lock: %s is set: this process runs in a container launched under "
            "a hold that keeps the window shut; run the timing job outside it" % COVERED_ENV)


def try_acquire(purpose, host=None, ttl_s=None, owner=None, _want_fd=None):
    """Take the window without blocking: a Holder, or None when another job
    holds or wants it (by lock or by a hand-written marker) or a launch holds
    it shut. Raises TimingLockError when this process's own ancestor holds it
    shut. `_want_fd` is the want lock acquire() already holds."""
    _upgrade(host)
    want = _want_fd if _want_fd is not None else _take_want(purpose, host)
    if want is None:
        return None

    def fail():
        if _want_fd is None:
            _close_want(want)
        return None
    path = lock_path(host)
    fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o644)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError as e:
        os.close(fd)
        if e.errno in (errno.EWOULDBLOCK, errno.EAGAIN):
            return fail()
        raise
    local = host in (None, "", "local", "windows")
    if local:
        line = _read(indicator_path())
        if line is not None and TAG not in line:
            fcntl.flock(fd, fcntl.LOCK_UN)       # a hand-written window is open
            os.close(fd)
            return fail()
    token = "%d.%d" % (os.getpid(), time.time_ns())
    info = {"token": token, "pid": os.getpid(), "purpose": purpose,
            "owner": owner or os.environ.get("OPENPETE_TIMING_OWNER") or
            os.environ.get("USER", "?"), "host": host or "local",
            "start": _now(), "ttl_s": ttl_s}
    os.ftruncate(fd, 0)
    os.pwrite(fd, (json.dumps(info) + "\n").encode(), 0)
    if local and not _publish_indicator(purpose, info["start"], token):
        fcntl.flock(fd, fcntl.LOCK_UN)           # a hand-written window opened
        os.close(fd)
        return fail()
    return Holder(fd, token, host, path, want)


def _publish_indicator(purpose, start, token):
    """Create TIMING-WINDOW for the holder of the local lock. link(2) fails
    on an existing file, so a hand-written marker created after the check in
    try_acquire is never overwritten; a tagged one is stale (this process
    holds the lock) and is replaced. False when a hand-written one exists."""
    ind = indicator_path()
    tmp = ind + ".%d.tmp" % os.getpid()
    with open(tmp, "w") as f:
        f.write("%s %s %s%s\n" % (purpose.replace(" ", "_"), start, TAG, token))
    try:
        for _ in range(2):
            try:
                os.link(tmp, ind)
                return True
            except FileExistsError:
                line = _read(ind)
                if line is None:
                    continue
                if TAG not in line:
                    return False
                os.unlink(ind)
        return False
    finally:
        os.unlink(tmp)


def _waiting(what, st, label, since, last):
    now = time.monotonic()
    if now - last[0] >= REPORT_EVERY_S or last[0] == 0:
        sys.stderr.write("timing-lock: %s%s waits (%.0f s): window %s\n" % (
            label + " " if label else "", what, now - since, describe(st)))
        last[0] = now


def acquire(purpose, host=None, timeout=None, ttl_s=None, owner=None):
    """Block until the window is taken; a Holder, or None at the timeout.
    The want lock is taken first and held while waiting, so new launches
    wait behind this job. Raises TimingLockError (see _upgrade)."""
    since, last = time.monotonic(), [0.0]
    _upgrade(host)
    want = None
    try:
        while want is None:
            want = _take_want(purpose, host)
            if want is None:
                if timeout is not None and time.monotonic() - since >= timeout:
                    return None
                _waiting("acquire for " + purpose, state(host), None, since, last)
                time.sleep(POLL_S)
        while True:
            h = try_acquire(purpose, host, ttl_s, owner, _want_fd=want)
            if h:
                want = None                       # the Holder owns it now
                return h
            if timeout is not None and time.monotonic() - since >= timeout:
                return None
            st = state(host)
            if not st["held"]:
                st = dict(st, held=True, readers=flock_hold.lockers(lock_path(host), "READ"))
            _waiting("acquire for " + purpose, st, None, since, last)
            time.sleep(POLL_S)
    finally:
        _close_want(want)


def wait_free(host=None, label=None, timeout=None):
    """Block while another job holds the window. True when free (or held by
    this job's token), False at the timeout."""
    since, last = time.monotonic(), [0.0]
    while True:
        st = state(host)
        if not st["held"] or st["mine"]:
            return True
        if timeout is not None and time.monotonic() - since >= timeout:
            sys.stderr.write("timing-lock: %sgave up after %.0f s: window %s\n"
                             % (label + " " if label else "", timeout, describe(st)))
            return False
        _waiting("launch", st, label, since, last)
        time.sleep(POLL_S)


_SHARED = {}            # host key -> this process's in-process flock_hold.Hold


def _key(host):
    return "local" if _local(host) else host


def gate(host=None, label=None, timeout=None, for_pid=None):
    """Wait out another job's window, then hold it shut: True, or False at
    the timeout.

    Returns at once when this process runs inside the window (its token is
    the holder's) or is covered by a hold of its own process tree (or
    OPENPETE_TIMING_COVERED is set). A caller that already holds it shut
    keeps its hold unless a timing job wants the window: then it releases,
    waits, and holds again. The hold is in-process (released at exit), or
    with `for_pid` a holder process serving that pid (a shell)."""
    path = lock_path(host)
    since = time.monotonic()
    me = for_pid or os.getpid()
    while True:
        st = state(host)
        if st["mine"]:
            return True
        own_holders = flock_hold.holders_for(path, me) if for_pid else []
        own = _SHARED.get(_key(host)) if not for_pid else None
        holding = bool(own_holders) or own is not None
        if holding and not st["held"]:
            return True
        if not holding:
            if os.environ.get(COVERED_ENV):
                return True
            if flock_hold.covered(path, flock_hold.lineage(me)[1:], "READ") is not None:
                return True
        # Entry, or a yield to a timing job that wants the window.
        for p in own_holders:
            flock_hold.stop(p)
        if own is not None:
            own.release()
            _SHARED.pop(_key(host), None)
        left = None if timeout is None else max(0.0, timeout - (time.monotonic() - since))
        if not wait_free(host, label, left):
            return False
        if for_pid:
            got = flock_hold.spawn(path, flock_hold.SH, for_pid, label or "")
        else:
            got = flock_hold.hold(path, flock_hold.SH)
            if got is not None:
                _SHARED[_key(host)] = got
        if got is not None and not state(host)["legacy"]:
            return True
        if got is not None and not for_pid:
            _SHARED.pop(_key(host)).release()
        elif got is not None:
            flock_hold.stop(got)
        if timeout is not None and time.monotonic() - since >= timeout:
            return False
        time.sleep(0.2)


def release_shared(host=None):
    """Release this process's in-process hold from gate(), if any."""
    h = _SHARED.pop(_key(host), None)
    if h is not None:
        h.release()


_PROCESS_HOLDER = None


def ensure_held(purpose, host=None, timeout=None):
    """Hold the window for the rest of this process unless this process
    already runs inside it (its token is the holder's); exports the token to
    children. Returns True when held, False at the timeout."""
    global _PROCESS_HOLDER
    if state(host)["mine"]:
        return True
    if _PROCESS_HOLDER is None:
        h = acquire(purpose, host, timeout)
        if h is None:
            return False
        _PROCESS_HOLDER = h
        os.environ[TOKEN_ENV] = h.token
    return True


# ---- display holder (Windows) ----------------------------------------------

# Wakes the display with a zero-distance mouse move (SetThreadExecutionState
# alone keeps an awake display on but does not turn a sleeping one on), then
# holds ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED until stdin
# reaches end of file. Each stdin line re-asserts the state and is answered
# with `display-hold: beat ret=<previous state>` (0 = the call failed), so the
# Linux side knows the holder is alive and its request in force; the
# process's exit returns the execution state.
DISPLAY_PS = r"""
$ErrorActionPreference = 'Stop'
Add-Type -Namespace OP -Name Pwr -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint f);
[DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, System.UIntPtr e);
'@
[OP.Pwr]::mouse_event(1, 0, 0, 0, [System.UIntPtr]::Zero)
$prev = [OP.Pwr]::SetThreadExecutionState([uint32]2147483651)
[Console]::Out.WriteLine("display-hold: on prev=$prev")
[Console]::Out.Flush()
while ($null -ne [Console]::In.ReadLine()) {
  $r = [OP.Pwr]::SetThreadExecutionState([uint32]2147483651)
  [Console]::Out.WriteLine("display-hold: beat ret=$r")
  [Console]::Out.Flush()
}
"""
DISPLAY_RECORD = "display-hold.json"
DISPLAY_BEAT_S = 20.0
# A record whose last answered beat is older than this is not a hold: the
# holder or its PowerShell child stopped answering.
DISPLAY_STALE_S = 3 * DISPLAY_BEAT_S


def display_record_path():
    """The display holder's record, <bench>/display-hold.json."""
    return os.path.join(bench_dir(), DISPLAY_RECORD)


class DisplayHold:
    """A running Windows display holder: the PowerShell child, the thread
    that re-asserts its request every DISPLAY_BEAT_S, and the record
    (display-hold.json: token, pid, since, beat, ret) a preflight reads to
    tell that the display is held."""

    def __init__(self, proc, token, ret):
        self.proc, self.token = proc, token
        self.since = time.time()
        self.stop = threading.Event()
        self._write(ret)
        self.thread = threading.Thread(target=self._beat_loop, daemon=True)
        self.thread.start()

    def _write(self, ret):
        rec = {"token": self.token, "pid": os.getpid(), "since": self.since,
               "beat": time.time(), "ret": ret}
        tmp = display_record_path() + ".%d.tmp" % os.getpid()
        with open(tmp, "w") as f:
            json.dump(rec, f)
        os.replace(tmp, display_record_path())

    def _beat_loop(self):
        while not self.stop.wait(DISPLAY_BEAT_S):
            try:
                self.proc.stdin.write("beat\n")
                self.proc.stdin.flush()
                line = self.proc.stdout.readline()
            except (OSError, ValueError):
                return
            if not line.startswith("display-hold: beat ret="):
                return                   # the child died; the record goes stale
            try:
                self._write(int(line.split("=", 1)[1]))
            except (OSError, ValueError):
                return

    def release(self):
        """Stop the beats, remove the record, close the child's stdin and
        reap it."""
        self.stop.set()
        try:
            rec = json.loads(_read(display_record_path()) or "{}")
            if rec.get("pid") == os.getpid() and rec.get("token") == self.token:
                os.unlink(display_record_path())
        except (OSError, ValueError):
            pass
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=30)
        except (OSError, ValueError, subprocess.TimeoutExpired):
            self.proc.kill()


def hold_display(token=None):
    """Start the Windows display holder for the window `token` (default this
    process's OPENPETE_TIMING_TOKEN); a DisplayHold, or None when
    powershell.exe does not run or does not take the request."""
    try:
        p = subprocess.Popen(["powershell.exe", "-NoProfile", "-NonInteractive",
                              "-Command", DISPLAY_PS],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, text=True)
    except OSError:
        return None
    line = p.stdout.readline()
    if not line.startswith("display-hold: on"):
        p.kill()
        return None
    try:
        ret = int(line.strip().rsplit("=", 1)[1])
    except (IndexError, ValueError):
        ret = 0
    return DisplayHold(p, token or os.environ.get(TOKEN_ENV), ret)


def release_display(hold):
    """Release a DisplayHold (None: nothing to release)."""
    if hold is not None:
        hold.release()


def _pid_alive(pid):
    try:
        os.kill(int(pid), 0)
    except (OSError, ValueError, TypeError):
        return False
    return True


def display_hold_state(now=None):
    """The display hold as a preflight reads it: {"held": bool, "why": text,
    "record": dict or None}. Held when display-hold.json exists, its token
    is the token of the job holding the local window, its writer is alive,
    its last beat is younger than DISPLAY_STALE_S, and the last
    SetThreadExecutionState call returned non-zero."""
    now = time.time() if now is None else now
    text = _read(display_record_path())
    if text is None:
        return {"held": False, "why": "no display holder (run the job under "
                "timing_lock.py run --hold-display)", "record": None}
    try:
        rec = json.loads(text)
    except ValueError:
        return {"held": False, "why": "display-hold.json unreadable", "record": None}
    st = state(None)
    tok = (st["lock"] or {}).get("token")
    age = now - float(rec.get("beat", 0))
    if not tok or rec.get("token") != tok:
        why = "the holder's token is not the window holder's"
    elif not _pid_alive(rec.get("pid")):
        why = "holder pid %s is gone" % rec.get("pid")
    elif age > DISPLAY_STALE_S:
        why = "last beat %.0f s ago (bound %.0f s)" % (age, DISPLAY_STALE_S)
    elif not rec.get("ret"):
        why = "SetThreadExecutionState returned 0"
    else:
        return {"held": True, "record": rec, "why": "held since %s by pid %s; last beat %.0f s "
                "ago (ES_DISPLAY_REQUIRED re-asserted every %.0f s)" % (
                    datetime.datetime.fromtimestamp(float(rec["since"])).astimezone()
                    .isoformat(timespec="seconds"), rec["pid"], age, DISPLAY_BEAT_S)}
    return {"held": False, "why": why, "record": rec}


# ---- commands ----------------------------------------------------------------

def cmd_run(a):
    if not a.cmd:
        sys.stderr.write("timing-lock: run needs -- CMD\n")
        return 64
    h = acquire(a.purpose, a.host, a.timeout)
    if h is None:
        sys.stderr.write("timing-lock: no window within %s s\n" % a.timeout)
        return EXIT_TIMEOUT
    sys.stderr.write("timing-lock: window held for %s (token %s)\n" % (a.purpose, h.token))
    disp = hold_display(h.token) if a.hold_display else None
    if a.hold_display and disp is None:
        sys.stderr.write("timing-lock: display holder did not start\n")
    env = dict(os.environ, **{TOKEN_ENV: h.token})
    child = subprocess.Popen(a.cmd, env=env)

    def forward(sig, _frame):
        child.send_signal(sig)
    for s in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(s, forward)
    rc = child.wait()
    release_display(disp)
    h.release()
    sys.stderr.write("timing-lock: window released (%s)\n" % a.purpose)
    return rc


def cmd_hold(a):
    """The detached holder behind `acquire`."""
    h = acquire(a.purpose, a.host, a.timeout, a.ttl)
    out = os.fdopen(a.ready_fd, "w")
    if h is None:
        out.write("timeout\n")
        out.close()
        return EXIT_TIMEOUT
    disp = hold_display(h.token) if a.hold_display else None
    out.write("%s %s\n" % (h.token, "display" if disp else "-"))
    out.close()
    stop = [False]
    signal.signal(signal.SIGTERM, lambda *_: stop.__setitem__(0, True))
    end = time.monotonic() + a.ttl
    while not stop[0] and time.monotonic() < end:
        if a.watch_pid and not os.path.isdir("/proc/%d" % a.watch_pid):
            break
        time.sleep(1)
    release_display(disp)
    h.release()
    return 0


def cmd_acquire(a):
    r, w = os.pipe()
    argv = [sys.executable, os.path.abspath(__file__), "_hold", "--purpose", a.purpose,
            "--ttl", str(a.ttl), "--ready-fd", str(w)]
    for k in ("host", "timeout", "watch_pid"):
        v = getattr(a, k)
        if v is not None:
            argv += ["--" + k.replace("_", "-"), str(v)]
    if a.hold_display:
        argv.append("--hold-display")
    # The holder outlives this command: it keeps none of the caller's
    # streams open, so a caller capturing this command's output returns.
    with open(os.path.join(bench_dir(), "timing-holder.log"), "a") as log:
        subprocess.Popen(argv, pass_fds=(w,), start_new_session=True,
                         stdin=subprocess.DEVNULL, stdout=log, stderr=log)
    os.close(w)
    with os.fdopen(r) as f:
        line = f.readline().strip()
    if not line or line == "timeout":
        sys.stderr.write("timing-lock: holder did not take the window\n")
        return EXIT_TIMEOUT
    token, disp = line.split()
    print("%s=%s" % (TOKEN_ENV, token))
    if a.hold_display and disp != "display":
        sys.stderr.write("timing-lock: display holder did not start\n")
    return 0


def cmd_release(a):
    st = state(a.host)
    info = st["lock"]
    if not info or info.get("token") != a.token:
        sys.stderr.write("timing-lock: token %s does not hold the window (%s)\n"
                         % (a.token, describe(st)))
        return 1
    pid = int(info["pid"])
    try:
        cmdline = _read("/proc/%d/cmdline" % pid) or ""
    except OSError:
        cmdline = ""
    if "_hold" not in cmdline:
        sys.stderr.write("timing-lock: pid %d is a `run` job, not a detached holder; "
                         "the window ends when it exits\n" % pid)
        return 1
    os.kill(pid, signal.SIGTERM)
    for _ in range(100):
        if not state(a.host)["lock"]:
            return 0
        time.sleep(0.1)
    sys.stderr.write("timing-lock: holder %d did not release\n" % pid)
    return 1


def cmd_status(a):
    st = state(a.host)
    print("timing window (%s): %s" % (a.host or "local", describe(st)))
    return 1 if st["held"] and not st["mine"] else 0


def cmd_wait(a):
    return 0 if wait_free(a.host, a.label, a.timeout) else EXIT_TIMEOUT


def cmd_gate(a):
    return 0 if gate(a.host, a.label, a.timeout, for_pid=a.for_pid) else EXIT_TIMEOUT


def cmd_shared(a):
    if not a.cmd:
        sys.stderr.write("timing-lock: shared needs -- CMD\n")
        return 64
    if not gate(a.host, a.label, a.timeout):
        return EXIT_TIMEOUT
    child = subprocess.Popen(a.cmd)

    def forward(sig, _frame):
        child.send_signal(sig)
    for s in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(s, forward)
    return child.wait()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd_name", required=True)

    def common(p):
        p.add_argument("--host", default=None, help="fleet host name (default: this machine)")
        p.add_argument("--timeout", type=float, default=None,
                       help="seconds to wait before exiting 75 (default: forever)")
    p = sub.add_parser("run")
    common(p)
    p.add_argument("--purpose", required=True)
    p.add_argument("--hold-display", action="store_true")
    p.add_argument("cmd", nargs=argparse.REMAINDER)
    p.set_defaults(fn=cmd_run)
    p = sub.add_parser("acquire")
    common(p)
    p.add_argument("--purpose", required=True)
    p.add_argument("--ttl", type=float, required=True)
    p.add_argument("--watch-pid", type=int, default=None)
    p.add_argument("--hold-display", action="store_true")
    p.set_defaults(fn=cmd_acquire)
    p = sub.add_parser("_hold")
    common(p)
    p.add_argument("--purpose", required=True)
    p.add_argument("--ttl", type=float, required=True)
    p.add_argument("--watch-pid", type=int, default=None)
    p.add_argument("--hold-display", action="store_true")
    p.add_argument("--ready-fd", type=int, required=True)
    p.set_defaults(fn=cmd_hold)
    p = sub.add_parser("release")
    p.add_argument("--host", default=None)
    p.add_argument("--token", required=True)
    p.set_defaults(fn=cmd_release)
    p = sub.add_parser("status")
    p.add_argument("--host", default=None)
    p.set_defaults(fn=cmd_status)
    p = sub.add_parser("wait")
    common(p)
    p.add_argument("--label", default=None)
    p.set_defaults(fn=cmd_wait)
    p = sub.add_parser("gate")
    common(p)
    p.add_argument("--label", default=None)
    p.add_argument("--for-pid", type=int, required=True)
    p.set_defaults(fn=cmd_gate)
    p = sub.add_parser("shared")
    common(p)
    p.add_argument("--label", default=None)
    p.add_argument("cmd", nargs=argparse.REMAINDER)
    p.set_defaults(fn=cmd_shared)
    a = ap.parse_args(argv)
    if getattr(a, "cmd", None) and a.cmd[:1] == ["--"]:
        a.cmd = a.cmd[1:]
    try:
        return a.fn(a)
    except TimingLockError as e:
        sys.stderr.write("%s\n" % e)
        return EXIT_NESTED


if __name__ == "__main__":
    sys.exit(main())
