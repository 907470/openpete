#!/usr/bin/env python3
"""Benchmark preflight: every checkable precondition of one timing leg, read
on the target it runs on, one line per check.

Targets: `local` (a Linux exe on this machine), `windows` (the Windows exe
launched from WSL), or a fleet host name from fleet-hosts.toml (`ptah`,
`steam-machine`).

Each check prints `preflight[<target>] <name>: <STATUS> <reading>`:
  PASS       read, and inside the bound
  FAIL       read, and outside it
  UNCHECKED  could not be read; never counted as a pass
  ACCEPTED   FAIL or UNCHECKED that the caller named with --accept
  INFO       recorded only (no bound applies)
A required check that is FAIL or UNCHECKED refuses the leg (exit 65) and the
last line names it. `--wait` re-reads the checks a running job can clear
(another holder, idle, load, golden lock) every 30 s instead of refusing on
them; it never launches while one fails.

Every leg declares its metric class (--class); the class decides what the
leg's arguments, the display and the finished leg's logs must show:
  cost     throughput, work per present or tick, a speed-up's size: --vsync 0
           (immediate) and --render-fps at least 5000, and post finds the
           measured present rate below 0.9 x the cap; or --timer, a GPU/CPU
           timer metric that does not depend on presents. Native size with
           --ssaa 1 unless --pixels declares otherwise.
  cadence  present intervals, lost presents, entry stalls: vsync on (fifo) at
           the display's refresh, the display held (--hold-display), every
           active monitor's mode equal to --baseline's; focus and fullscreen
           are not required. --static: post requires the steady present rate
           at least 0.99 x refresh.
  boot     first tick, compile stage: no present requirement.

Checks:
  lock          this job holds the timing window (scripts/bench/timing_lock.py)
  tuned         the exe's build tree is linux-release* / windows-mingw* without
                -fsanitize (a Windows-local copy is judged by --source-exe, and
                must be byte-identical to it)
  idle-linux    no game, emulator, compiler, linker or profiler by process name
  idle-windows  no openpete / duckstation exe on the Windows side (tasklist)
  load-linux    CPU busy over 2 s (/proc/stat) at most 10% of all cores, and no
                single process above 50% of one core (/proc/<pid>/stat)
  load-windows  Windows CPU load, 3 s typeperf mean, at most 20%
  golden-lock   the fleet golden lock of the target host is free
  display       held: the job's display holder (timing_lock.py --hold-display)
                re-asserts ES_DISPLAY_REQUIRED every 20 s and its record
                (<bench>/display-hold.json) carries the window holder's token,
                a live writer and a beat under 60 s old. Without a hold the
                state is inferred from the last input against the display-off
                timeout, which a held display outlives. cadence: held
                required; cost: held or inferred awake; boot, --timer: INFO
  monitors      every active display's mode (Screen.AllScreens,
                EnumDisplaySettings), recorded on every Windows leg; cadence:
                equal to the --baseline record's set (written when absent)
  windows-local the exe and every existing path argument live on a Windows
                drive (/mnt/<letter>/); a path on the Linux side is read over
                9P at Windows run time
  window        the requested window size is honoured by the arguments:
                fullscreen with --background keeps the boot window, and
                fullscreen ignores --window
  present-mode  the present mode the leg asks for (INFO)
  class         the arguments against the declared class
  pixels        the declared render size (--pixels WxH: [render-res] T=, SSAA
                included); a cost leg without it must be native, --ssaa 1
  neighbours    the exe folder: assets/, a texture pack (assets/textures,
                --texpack-dir), a config (openpete.toml read without
                --no-config, --config), enabled mods (mods/enabled.txt,
                --mods), openpete.opts / .env (not read by the exe); a staged
                texture pack, config or mod set must be declared (--staged)
  mem           MemAvailable (ptah: at least 8 GiB, its xe driver leaks
                GPUActive memory per process); GPUActive is INFO
  gpu-clock     GPU max frequency equals RP0 (no cap); current is INFO
  power-profile ACPI platform profile not low-power / quiet / cool
  governor      CPU frequency governor (INFO)

Post checks (the leg's log and its pace log; the main exe prints no
present-mode line under fifo and no swapchain-extent line):
  window-actual        [render-res] C= of every live line against --window
  pixels-actual        the last [render-res] T= against the declaration
  present-mode-actual  the pace log env's present_mode against the class
  refresh-actual       refresh_mhz, fullscreen, render_fps (INFO)
  steady-rate          cadence --static: presents/s over the last 10 s
  cap-headroom         cost: presents/s below 0.9 x render_fps

Commands:
  check [--target T] [--exe E] DECL [-- leg args]   print the checks; exit 0/65
  leg   [--target T] [--log L] [--wait] DECL -- CMD... preflight, run CMD, then
        the post checks on L (pace log: --pace-log, else the leg's own
        --pace-log, a Windows path read through /mnt); refuses (65) without
        running CMD, and exits 65 when CMD succeeded but post refuses
  post  --log L DECL [--expect-window WxH]          the post checks alone
  DECL: --class cost|cadence|boot [--timer] [--pixels WxH]
        [--staged texpack,config,mods] [--baseline REC] [--record REC] [--static]
        --record writes the leg's record (JSON: arguments, declaration,
        monitors, neighbours, checks; post appends its checks)

Exit: 0 every required check passed, 65 refused, 64 usage.
"""
import argparse
import csv
import fcntl
import hashlib
import importlib.util
import io
import json
import os
import re
import shlex
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "scripts", "fences"))
import timing_lock                                          # noqa: E402

EXIT_REFUSED = 65
LOAD_PCT = 10.0
# A single process above this share of one core fails the load check even
# when the machine-wide share is under LOAD_PCT (one busy thread on 16 cores
# is 6%).
HOT_PROC_PCT = 50.0
LOAD_SAMPLE_S = 2.0
WIN_LOAD_PCT = 20.0
PTAH_MEM_MIN_KB = 8 * 1024 * 1024
MEM_MIN_KB = {"ptah": PTAH_MEM_MIN_KB}
# ACPI platform_profile values that cap the package power (power-profiles-
# daemon's power-saver is low-power there).
LOW_POWER_PROFILES = {"low-power", "quiet", "cool", "power-saver"}
WAIT_POLL_S = 30
# Checks a running job can clear by finishing; `--wait` re-reads these. The
# lock check is waitable only while another job holds the window.
WAITABLE = {"idle-linux", "idle-windows", "load-linux", "load-windows",
            "golden-lock", "idle", "load"}

# Exact Linux process names (15-character comm) that load the machine: game
# processes, the emulator, compilers and linkers, the build drivers, and
# profilers. A sanitizer or instrumented game binary keeps an openpete name.
LINUX_BUSY_RE = (r"openpete|openpete-spyro[0-9]|duckstation.*|cc1|cc1plus|clang.*"
                 r"|ld|ld\.lld|lld|ninja|make|memcheck-.*|callgrind-.*|cachegrind-.*"
                 r"|perf|podman|conmon|x86_64-w64-min.*")
WIN_BUSY_RE = r"openpete(-spyro[0-9])?\.exe|duckstation.*\.exe"
OPENPETE_EXE_RE = re.compile(r"(^|/)openpete(-spyro[0-9])?(\.exe)?$")


class Check:
    """One precondition's reading."""

    def __init__(self, name, status, reading, required=True):
        self.name, self.status, self.reading, self.required = name, status, reading, required

    def waitable(self):
        """True when another job's finish can clear this failure."""
        return self.name in WAITABLE or (self.name == "lock" and self.reading.startswith("held"))

    def blocks(self):
        """True when this check refuses the leg."""
        return self.required and self.status in ("FAIL", "UNCHECKED")


def _run(argv, timeout=30, **kw):
    """(returncode, stdout) of argv, or (None, "") when it cannot run."""
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout, **kw)
    except (OSError, subprocess.TimeoutExpired):
        return None, ""
    return r.returncode, r.stdout


# ---- leg arguments -----------------------------------------------------------

def split_leg(cmd):
    """(exe, game args, env) from a leg command: the first word naming an
    openpete binary, the words after it, and the KEY=VALUE words before it
    (an `env` prefix included); exe None when none is named."""
    env = {}
    for i, w in enumerate(cmd):
        if OPENPETE_EXE_RE.search(w):
            return w, cmd[i + 1:], env
        if re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", w):
            k, v = w.split("=", 1)
            env[k] = v
    return None, list(cmd), env


def _flag(args, name):
    """The value of `--name V` / `--name=V`; "" for a bare flag; None absent."""
    for i, w in enumerate(args):
        if w == name:
            nxt = args[i + 1] if i + 1 < len(args) else ""
            return "" if nxt.startswith("--") else nxt
        if w.startswith(name + "="):
            return w[len(name) + 1:]
    return None


def _onoff(v):
    if v is None:
        return None
    return v in ("", "1", "on", "true", "yes")


def _sidecar_fullscreen(exe):
    """True/False from `fullscreen = ...` in the openpete.toml beside the exe,
    None when there is none."""
    if not exe:
        return None
    p = os.path.join(os.path.dirname(os.path.abspath(exe)), "openpete.toml")
    try:
        text = open(p, errors="replace").read()
    except OSError:
        return None
    m = re.search(r"^\s*fullscreen\s*=\s*(\w+)", text, re.M)
    return None if not m else m.group(1) == "true"


def check_window(exe, args, env):
    """The window check from the leg's arguments and environment."""
    if exe is None:
        return Check("window", "UNCHECKED", "no openpete exe in the leg command", required=False)
    if _flag(args, "--headless") is not None:
        return Check("window", "PASS", "headless: no window")
    fs = _onoff(_flag(args, "--fullscreen"))
    src = "--fullscreen"
    if fs is None and _flag(args, "--no-config") is None:
        fs, src = _sidecar_fullscreen(exe), "openpete.toml beside the exe"
    bg = _onoff(_flag(args, "--background"))
    if bg is None:
        bg = _onoff(env.get("OPENPETE_BACKGROUND", os.environ.get("OPENPETE_BACKGROUND")))
    win = _flag(args, "--window")
    if fs and bg:
        return Check("window", "FAIL", "fullscreen (%s) with background mode keeps the "
                     "boot window size; pass --fullscreen 0 --window WxH" % src)
    if fs and win:
        return Check("window", "FAIL", "fullscreen (%s) ignores --window %s" % (src, win))
    if fs:
        return Check("window", "PASS", "fullscreen (%s), desktop size" % src)
    if not win:
        return Check("window", "UNCHECKED", "windowed with no --window: size is the "
                     "default or the saved one")
    return Check("window", "PASS", "windowed %s (post reads [render-res] C=)" % win)


def requested_present_mode(args, env):
    """The present mode the leg asks for: --vsync 1 -> fifo, --vsync 0 ->
    immediate (the exe falls back to mailbox, then fifo, when the surface
    lacks it), else OPENPETE_PRESENT_MODE, else fifo."""
    vs = _onoff(_flag(args, "--vsync"))
    if vs is not None:
        return "fifo" if vs else "immediate"
    pm = env.get("OPENPETE_PRESENT_MODE", os.environ.get("OPENPETE_PRESENT_MODE"))
    return pm if pm in ("immediate", "mailbox") else "fifo"


# ---- metric classes ----------------------------------------------------------
#
# cost     throughput, work per present or tick, a speed-up's size: presents
#          unthrottled (immediate, --render-fps at least COST_MIN_CAP), or with
#          --timer a GPU/CPU timer metric that does not depend on presents.
# cadence  present intervals, lost presents, entry stalls: vsync on at the
#          display's refresh, the display held, the monitor set equal to the
#          baseline's; focus and fullscreen are not required.
# boot     first tick, compile stage: no present requirement.
CLASSES = ("cost", "cadence", "boot")
COST_MIN_CAP = 5000
# post: a cost leg whose measured present rate reaches this share of its
# --render-fps cap was bound by the cap, not by the work.
COST_CAP_SHARE = 0.9
# post: a cadence leg's static scene must present at least this share of
# the refresh rate.
CADENCE_MIN_SHARE = 0.99
CADENCE_STEADY_S = 10.0


def display_need(cls, timer):
    """What the display check requires of a class: "held", "awake" or None."""
    if cls == "cadence":
        return "held"
    if cls == "cost" and not timer:
        return "awake"
    return None


def check_class(cls, timer, args, env, refresh_hz=None):
    """The leg's arguments against its declared metric class."""
    if cls is None:
        return Check("class", "FAIL", "no --class (cost | cadence | boot)")
    if cls not in CLASSES:
        return Check("class", "FAIL", "unknown class %r (cost | cadence | boot)" % cls)
    if _flag(args, "--headless") is not None and cls != "boot" and not timer:
        return Check("class", "FAIL", "%s leg runs --headless: no display presents" % cls)
    pm = requested_present_mode(args, env)
    fps = _flag(args, "--render-fps")
    if cls == "boot" or (cls == "cost" and timer):
        return Check("class", "PASS", "%s%s: no present requirement (present mode %s)"
                     % (cls, " --timer" if timer else "", pm))
    if cls == "cost":
        bad = []
        if pm != "immediate":
            bad.append("present mode %s (pass --vsync 0)" % pm)
        if not (fps and fps.isdigit() and int(fps) >= COST_MIN_CAP):
            bad.append("--render-fps %s (pass at least %d)" % (fps or "unset", COST_MIN_CAP))
        if bad:
            return Check("class", "FAIL", "cost: " + "; ".join(bad))
        return Check("class", "PASS", "cost: immediate, --render-fps %s" % fps)
    bad = []
    if pm != "fifo":
        bad.append("present mode %s (cadence is vsync on: --vsync 1)" % pm)
    if fps is not None and not (fps.isdigit() and refresh_hz and abs(int(fps) - refresh_hz) <= 1):
        bad.append("--render-fps %s is not the display's refresh %s Hz (omit it)"
                   % (fps or "''", refresh_hz or "?"))
    if bad:
        return Check("class", "FAIL", "cadence: " + "; ".join(bad))
    return Check("class", "PASS", "cadence: fifo at the refresh (%s Hz)" % (refresh_hz or "?"))


def parse_wxh(v):
    """(w, h) from "WxH", or None."""
    m = re.fullmatch(r"(\d+)x(\d+)", v or "")
    return (int(m.group(1)), int(m.group(2))) if m else None


def check_pixels(cls, timer, args, pixels):
    """The render size the leg declares. `pixels` is --pixels WxH (the render
    target the log's [render-res] T= must show, SSAA included). A cost leg
    without it declares the native size: no --resolution and --ssaa 1."""
    ssaa = _flag(args, "--ssaa")
    res = _flag(args, "--resolution")
    if pixels:
        wh = parse_wxh(pixels)
        if not wh:
            return Check("pixels", "FAIL", "--pixels %r is not WxH" % pixels)
        return Check("pixels", "PASS", "declared %s = %d px (post reads [render-res] T=)"
                     % (pixels, wh[0] * wh[1]))
    if cls == "cost" and not timer and ((ssaa not in (None, "1")) or
                                        (res not in (None, "", "window"))):
        return Check("pixels", "FAIL", "cost leg with --ssaa %s --resolution %s declares no "
                     "--pixels; cost legs default to native size with --ssaa 1"
                     % (ssaa or "unset", res or "unset"))
    what = "native size (the window), --ssaa 1" if cls == "cost" else "not declared"
    return Check("pixels", "PASS" if cls == "cost" else "INFO",
                 "%s (post records [render-res] T=)" % what, required=cls == "cost")


# ---- exe folder neighbours ---------------------------------------------------

STAGEABLE = ("texpack", "config", "mods")


def _enabled_mods(path):
    try:
        with open(path, errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return []
    return [l.strip() for l in lines if l.strip() and not l.strip().startswith("#")]


def exe_neighbours(exe, args, env):
    """What sits beside the exe and what the leg's arguments point at:
    {"assets": [subdirs], "texpack": reason or None, "config": reason or None,
     "mods": reason or None, "retired": [files]}."""
    d = os.path.dirname(os.path.abspath(exe))
    out = {"dir": d, "assets": [], "retired": []}
    assets = os.path.join(d, "assets")
    if os.path.isdir(assets):
        out["assets"] = sorted(os.listdir(assets))
    tex = os.path.join(assets, "textures")
    texdir = _flag(args, "--texpack-dir") or env.get("OPENPETE_TEXPACK_DIR")
    if texdir:
        out["texpack"] = "texture pack dir %s" % texdir
    elif os.path.isdir(tex) and os.listdir(tex):
        out["texpack"] = "assets/textures holds %d entries" % len(os.listdir(tex))
    else:
        out["texpack"] = None
    no_cfg = _flag(args, "--no-config") is not None
    cfg = _flag(args, "--config")
    if cfg:
        out["config"] = "--config %s" % cfg
    elif os.path.isfile(os.path.join(d, "openpete.toml")) and not no_cfg:
        out["config"] = "openpete.toml beside the exe, read (no --no-config)"
    else:
        out["config"] = None
    # --mods "" loads no mod; an unset --mods loads the mods dir's enabled.txt.
    mods_arg = _flag(args, "--mods")
    mods_dir = _flag(args, "--mods-dir") or env.get("OPENPETE_MODS_DIR") or \
        os.path.join(d, "mods")
    mods = _enabled_mods(os.path.join(mods_dir, "enabled.txt"))
    if mods_arg:
        out["mods"] = "--mods %s" % mods_arg
    elif mods and mods_arg is None:
        out["mods"] = "%s/enabled.txt: %s" % (mods_dir, ",".join(mods[:6]))
    else:
        out["mods"] = None
    out["retired"] = [f for f in ("openpete.opts", "openpete.env")
                      if os.path.isfile(os.path.join(d, f))]
    return out


def check_neighbours(exe, args, env, staged):
    """The exe folder's neighbours against what the leg declares staged
    (--staged texpack,config,mods): an undeclared texture pack, config or
    mod set refuses the leg, and so does a declared one that is absent."""
    if not exe:
        return Check("neighbours", "UNCHECKED", "no exe named")
    n = exe_neighbours(exe, args, env)
    bad, parts = [], []
    for k in STAGEABLE:
        if n[k] and k not in staged:
            bad.append("%s staged but not declared: %s" % (k, n[k]))
        elif not n[k] and k in staged:
            bad.append("%s declared but none staged" % k)
        elif n[k]:
            parts.append("%s (declared): %s" % (k, n[k]))
    parts.append("assets/: %s" % (",".join(n["assets"]) or "none"))
    if n["retired"]:
        parts.append("%s present, not read by the exe" % ",".join(n["retired"]))
    if bad:
        return Check("neighbours", "FAIL", "; ".join(bad + parts))
    return Check("neighbours", "PASS", "; ".join(parts))


# ---- tuned -------------------------------------------------------------------

def _sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def check_tuned(exe, source_exe=None):
    """The tuned-build check; a copy outside any build tree is judged by
    `source_exe`, which it must match byte for byte."""
    import bench_guard
    if not exe:
        return Check("tuned", "UNCHECKED", "no exe named")
    judged = exe
    note = ""
    if source_exe:
        try:
            if _sha(exe) != _sha(source_exe):
                return Check("tuned", "FAIL", "%s differs from its source %s" % (exe, source_exe))
        except OSError as e:
            return Check("tuned", "UNCHECKED", "cannot compare %s with %s: %s"
                         % (exe, source_exe, e))
        judged, note = source_exe, " (copy of %s)" % source_exe
    cache = bench_guard._build_cache(judged)
    if not cache:
        return Check("tuned", "UNCHECKED", "no CMakeCache.txt above %s%s; pass --source-exe "
                     "for a staged copy" % (judged, note))
    include, flags = bench_guard._cache_entries(cache)
    base = os.path.basename(include or "")
    if not re.match(r"(linux-release|windows-mingw).*\.cmake$", base):
        return Check("tuned", "FAIL", "preset include %r in %s" % (base or "(none)", cache))
    if any("-fsanitize" in f for f in flags):
        return Check("tuned", "FAIL", "-fsanitize in %s" % cache)
    return Check("tuned", "PASS", "%s%s" % (base[:-len(".cmake")], note))


# ---- local and Windows -------------------------------------------------------

def check_lock(host):
    st = timing_lock.state(host)
    if st["mine"]:
        return Check("lock", "PASS", timing_lock.describe(st))
    if st["held"]:
        return Check("lock", "FAIL", timing_lock.describe(st))
    return Check("lock", "FAIL", "free: this job holds no window (run it under "
                 "scripts/bench/timing_lock.py run)")


def _token_pids():
    """PIDs whose environment carries this job's timing token: the job's own
    processes, which are not contention."""
    tok = os.environ.get(timing_lock.TOKEN_ENV)
    pids = set()
    if not tok:
        return pids
    needle = ("%s=%s" % (timing_lock.TOKEN_ENV, tok)).encode()
    for d in os.listdir("/proc"):
        if d.isdigit():
            try:
                with open("/proc/%s/environ" % d, "rb") as f:
                    if needle in f.read().split(b"\0"):
                        pids.add(d)
            except OSError:
                pass
    return pids


def check_idle_linux():
    rc, out = _run(["pgrep", "-ax", LINUX_BUSY_RE], timeout=10)
    if rc is None:
        return Check("idle-linux", "UNCHECKED", "pgrep did not run")
    mine = _token_pids()
    busy = [l for l in out.splitlines() if l.strip() and l.split()[0] not in mine]
    if busy:
        return Check("idle-linux", "FAIL", "%d busy: %s" % (
            len(busy), "; ".join(l[:90] for l in busy[:4])))
    return Check("idle-linux", "PASS", "no game, emulator, compiler or profiler process")


def _cpu_times(text):
    """(busy, total) jiffies from the `cpu ` line of /proc/stat text."""
    for line in text.splitlines():
        if line.startswith("cpu "):
            v = [int(x) for x in line.split()[1:]]
            idle = v[3] + (v[4] if len(v) > 4 else 0)
            return sum(v) - idle, sum(v)
    raise ValueError("no cpu line")


def busy_pct(before, after):
    """CPU busy percent of all cores between two /proc/stat readings."""
    b0, t0 = _cpu_times(before)
    b1, t1 = _cpu_times(after)
    return 100.0 * (b1 - b0) / max(1, t1 - t0)


def proc_ticks(text):
    """{pid: (comm, utime+stime ticks)} from concatenated /proc/<pid>/stat lines."""
    out = {}
    for line in text.splitlines():
        head, sep, tail = line.rpartition(") ")
        if not sep or " (" not in head:
            continue
        pid, comm = head.split(" (", 1)
        f = tail.split()
        try:
            out[pid.strip()] = (comm, int(f[11]) + int(f[12]))
        except (IndexError, ValueError):
            pass
    return out


def hot_processes(before, after, seconds, exclude=(), hz=100):
    """[(pct of one core, pid, comm)] above HOT_PROC_PCT between two
    proc_ticks maps, hottest first."""
    hot = []
    for pid, (comm, t1) in after.items():
        if pid in exclude or pid not in before:
            continue
        pct = 100.0 * (t1 - before[pid][1]) / hz / seconds
        if pct > HOT_PROC_PCT:
            hot.append((pct, pid, comm))
    return sorted(hot, reverse=True)


def load_check(name, before, after, load1, cores, hot=()):
    """The load check from two /proc/stat readings LOAD_SAMPLE_S apart; the
    1-minute load average is recorded but not judged, since it still holds
    the previous leg's process for a minute after it exits."""
    try:
        pct = busy_pct(before, after)
    except (ValueError, IndexError):
        return Check(name, "UNCHECKED", "/proc/stat unreadable")
    st = "PASS" if pct <= LOAD_PCT and not hot else "FAIL"
    hot_s = "; above %.0f%% of a core: %s" % (HOT_PROC_PCT, ", ".join(
        "%s[%s] %.0f%%" % (c, p, v) for v, p, c in hot[:4])) if hot else ""
    return Check(name, st, "%.1f%% busy over %.0f s on %s cores (bound %.0f%%); load1 %s%s"
                 % (pct, LOAD_SAMPLE_S, cores, LOAD_PCT, load1, hot_s))


def _local_proc_stats():
    lines = []
    for d in os.listdir("/proc"):
        if d.isdigit():
            try:
                with open("/proc/%s/stat" % d) as f:
                    lines.append(f.read())
            except OSError:
                pass
    return "".join(lines)


def check_load_linux():
    try:
        before, pb = open("/proc/stat").read(), proc_ticks(_local_proc_stats())
        time.sleep(LOAD_SAMPLE_S)
        after, pa = open("/proc/stat").read(), proc_ticks(_local_proc_stats())
        load1 = open("/proc/loadavg").read().split()[0]
    except OSError:
        return Check("load-linux", "UNCHECKED", "/proc/stat unreadable")
    hot = hot_processes(pb, pa, LOAD_SAMPLE_S,
                        _token_pids() | {str(os.getpid())}, os.sysconf("SC_CLK_TCK"))
    return load_check("load-linux", before, after, load1, os.cpu_count(), hot)


def check_idle_windows():
    rc, out = _run(["tasklist.exe", "/FO", "CSV", "/NH"], timeout=30)
    if rc is None or not out.strip():
        return Check("idle-windows", "UNCHECKED", "tasklist.exe did not run")
    names = [r[0] for r in csv.reader(io.StringIO(out.replace("\r", ""))) if r]
    busy = sorted({n for n in names if re.fullmatch(WIN_BUSY_RE, n, re.I)})
    if busy:
        return Check("idle-windows", "FAIL", "running: " + ", ".join(busy))
    return Check("idle-windows", "PASS", "no openpete or duckstation exe (%d processes)" % len(names))


def check_load_windows():
    import bench_guard
    load = bench_guard._windows_cpu_load()
    if load is None:
        return Check("load-windows", "UNCHECKED", "typeperf.exe printed no sample")
    st = "PASS" if load <= WIN_LOAD_PCT else "FAIL"
    return Check("load-windows", st, "%.1f%% over %d s (bound %.0f%%; the WSL VM counts)"
                 % (load, bench_guard.WIN_LOAD_SAMPLES, WIN_LOAD_PCT))


def _fleet_hosts():
    """{name: config} from the fleet host file, {} when it cannot be read."""
    try:
        spec = importlib.util.spec_from_file_location(
            "golden_fleet", os.path.join(ROOT, "scripts", "golden-fleet.py"))
        gf = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gf)
        return gf.load_hosts(required=False)[1]
    except Exception:
        return {}


def check_golden_lock_local(hosts):
    cfg = next((c for c in hosts.values() if c.get("transport") == "local"), None)
    if cfg is None:
        return Check("golden-lock", "UNCHECKED", "no local host in fleet-hosts.toml")
    path = os.path.join(os.path.expanduser(cfg.get("fleet_root", "~/.cache/openpete-fleet")),
                        "golden.lock")
    if not os.path.exists(path):
        return Check("golden-lock", "PASS", "free (no %s)" % path)
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC)
    try:
        fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
        fcntl.flock(fd, fcntl.LOCK_UN)
        return Check("golden-lock", "PASS", "free")
    except OSError:
        holder = (open(path, errors="replace").read(200) or "").replace("\n", " ").strip()
        return Check("golden-lock", "FAIL", "held: " + holder)
    finally:
        os.close(fd)


# One PowerShell call for the display state and every active monitor's mode:
# Screen.AllScreens names the active displays, EnumDisplaySettings
# (ENUM_CURRENT_SETTINGS) reads each one's mode. Win32_VideoController is not
# used: it reports one mode per adapter, not per monitor.
DISPLAY_QUERY_PS = r"""
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -Namespace OP -Name In -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct LII { public uint cb; public uint t; }
[DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LII p);
[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct DEVMODE {
  [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
  public short dmSpecVersion; public short dmDriverVersion; public short dmSize; public short dmDriverExtra;
  public int dmFields; public int dmPositionX; public int dmPositionY; public int dmDisplayOrientation; public int dmDisplayFixedOutput;
  public short dmColor; public short dmDuplex; public short dmYResolution; public short dmTTOption; public short dmCollate;
  [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
  public short dmLogPixels; public int dmBitsPerPel; public int dmPelsWidth; public int dmPelsHeight;
  public int dmDisplayFlags; public int dmDisplayFrequency; public int dmICMMethod; public int dmICMIntent;
  public int dmMediaType; public int dmDitherType; public int dmReserved1; public int dmReserved2;
  public int dmPanningWidth; public int dmPanningHeight;
}
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool EnumDisplaySettings(string dev, int mode, ref DEVMODE dm);
'@
$l = New-Object OP.In+LII; $l.cb = 8
[void][OP.In]::GetLastInputInfo([ref]$l)
"idle_ms=" + ([uint32][Environment]::TickCount - $l.t)
$q = powercfg /q SCHEME_CURRENT SUB_VIDEO VIDEOIDLE | Out-String
if ($q -match 'Current AC Power Setting Index:\s*0x([0-9a-fA-F]+)') { "videoidle_ac=" + [Convert]::ToInt64($matches[1], 16) }
foreach ($s in [System.Windows.Forms.Screen]::AllScreens) {
  $d = New-Object OP.In+DEVMODE; $d.dmSize = [System.Runtime.InteropServices.Marshal]::SizeOf($d)
  if ([OP.In]::EnumDisplaySettings($s.DeviceName, -1, [ref]$d)) {
    "monitor=" + $s.DeviceName + "|" + [int]$s.Primary + "|" + $d.dmPelsWidth + "x" + $d.dmPelsHeight + "|" + $d.dmDisplayFrequency + "|" + $d.dmBitsPerPel + "|" + $s.Bounds.X + "," + $s.Bounds.Y
  } else { "monitor=" + $s.DeviceName + "|" + [int]$s.Primary + "|?|?|?|?" }
}
"""


def parse_display_state(out):
    """{"idle_ms", "videoidle_ac", "monitors": [{"device", "primary", "mode",
    "hz", "bpp", "pos"}]} from the display query's output, or None without
    an idle_ms line."""
    d = {"monitors": []}
    for line in out.replace("\r", "").splitlines():
        k, _, v = line.partition("=")
        if k == "monitor":
            f = (v.split("|") + ["?"] * 6)[:6]
            d["monitors"].append({"device": f[0], "primary": f[1] == "1", "mode": f[2],
                                  "hz": f[3], "bpp": f[4], "pos": f[5]})
        elif k in ("idle_ms", "videoidle_ac"):
            try:
                d[k] = int(v)
            except ValueError:
                pass
    return d if "idle_ms" in d else None


def windows_display_state():
    """parse_display_state of the Windows query, or None when it fails."""
    rc, out = _run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                    DISPLAY_QUERY_PS], timeout=60)
    return parse_display_state(out) if rc == 0 else None


def _inferred_awake(ds):
    """(awake True/False/None, text) from the last-input time against the
    display-off timeout."""
    if ds is None:
        return None, "powershell.exe query failed"
    idle = ds["idle_ms"] / 1000.0
    to = ds.get("videoidle_ac")
    if to is None:
        return None, "last input %.0f s ago; display-off timeout unreadable" % idle
    if to == 0:
        return True, "display-off timeout never; last input %.0f s ago" % idle
    return idle < to, "last input %.0f s ago, display-off timeout %d s" % (idle, to)


def check_display(ds, need, hold=None):
    """The display check. `need` is "held" (cadence: the job's display
    holder must be running, read from timing_lock.display_hold_state), "awake"
    (held, else awake by the last-input inference) or None (recorded only).
    The last-input inference cannot see a held display: a held display stays
    on while the last input ages past the display-off timeout."""
    hold = hold if hold is not None else timing_lock.display_hold_state()
    awake, inferred = _inferred_awake(ds)
    if hold["held"]:
        return Check("display", "PASS", "held: %s" % hold["why"], need is not None)
    if need == "held":
        return Check("display", "FAIL", "not held: %s; inferred %s" % (hold["why"], inferred))
    if need is None:
        return Check("display", "INFO", "not held; inferred %s" % inferred, required=False)
    if awake is None:
        return Check("display", "UNCHECKED", "not held; %s" % inferred)
    return Check("display", "PASS" if awake else "FAIL", "not held; %s (inferred: %s); "
                 "--hold-display holds it" % ("awake" if awake else "asleep", inferred))


def monitor_set(ds):
    """The comparable monitor set: sorted (device, primary, mode, hz)."""
    return sorted((m["device"], m["primary"], m["mode"], m["hz"]) for m in ds["monitors"])


def _monitor_text(mons):
    return "; ".join("%s%s %s@%s Hz" % (m["device"].replace("\\\\.\\", ""),
                                        " (primary)" if m["primary"] else "", m["mode"], m["hz"])
                     for m in mons) or "none"


def primary_hz(ds):
    """The primary monitor's refresh in Hz, or None."""
    for m in (ds or {}).get("monitors", []):
        if m["primary"] and m["hz"].isdigit():
            return int(m["hz"])
    return None


def check_monitors(ds, baseline, required):
    """Every active monitor's mode; with `baseline` (a leg record path) the
    set must equal the one recorded there. A missing baseline file is written
    from this reading. The set is a recorded condition of a cadence result:
    a mode change on any active display changes what the compositor does
    each refresh."""
    if ds is None or not ds["monitors"]:
        return Check("monitors", "UNCHECKED", "no monitor read (Screen.AllScreens / "
                     "EnumDisplaySettings)", required)
    now = _monitor_text(ds["monitors"])
    if not baseline:
        if required:
            return Check("monitors", "FAIL", "%s; no --baseline to compare with" % now)
        return Check("monitors", "INFO", now, required=False)
    try:
        with open(baseline) as f:
            base = json.load(f)["monitors"]
    except FileNotFoundError:
        write_record(baseline, {"monitors": ds["monitors"], "written": _now_iso(),
                                "as": "baseline"})
        return Check("monitors", "PASS", "%s; recorded as the baseline %s" % (now, baseline),
                     required)
    except (OSError, ValueError, KeyError, TypeError) as e:
        return Check("monitors", "UNCHECKED", "baseline %s unreadable: %s" % (baseline, e),
                     required)
    same = monitor_set({"monitors": base}) == monitor_set(ds)
    return Check("monitors", "PASS" if same else "FAIL", "%s; baseline %s: %s" % (
        now, "same" if same else "differs", _monitor_text(base)), required)


def _now_iso():
    return time.strftime("%Y-%m-%dT%H:%M:%S%z")


def write_record(path, rec):
    """Write a leg record (JSON) atomically."""
    d = os.path.dirname(os.path.abspath(path))
    os.makedirs(d, exist_ok=True)
    tmp = path + ".%d.tmp" % os.getpid()
    with open(tmp, "w") as f:
        json.dump(rec, f, indent=1, sort_keys=True)
    os.replace(tmp, path)


def check_windows_local(exe, args):
    if not exe:
        return Check("windows-local", "UNCHECKED", "no exe named")
    bad = []
    for p in [exe] + [a for a in args if not a.startswith("-")]:
        if p is exe or os.path.exists(p):
            real = os.path.realpath(p)
            if not re.match(r"^/mnt/[a-z]/", real):
                bad.append(real)
    if bad:
        return Check("windows-local", "FAIL", "read over 9P at run time: " + ", ".join(bad[:4]))
    return Check("windows-local", "PASS", "exe and path arguments on a Windows drive")


# ---- fleet host --------------------------------------------------------------

REMOTE_SH = r"""
echo "nproc=$(nproc)"
echo "loadavg=$(cut -d' ' -f1 /proc/loadavg)"
echo "stat0=$(grep '^cpu ' /proc/stat)"; cat /proc/[0-9]*/stat 2>/dev/null | sed 's/^/pstat0=/'
sleep 2
echo "stat1=$(grep '^cpu ' /proc/stat)"; cat /proc/[0-9]*/stat 2>/dev/null | sed 's/^/pstat1=/'
echo "clk_tck=$(getconf CLK_TCK)"
grep -E '^(MemAvailable|GPUActive):' /proc/meminfo | sed -E 's/: +/=/; s/ kB$//'
lk="$FLEET_ROOT/golden.lock"
if [ ! -e "$lk" ] || flock -n "$lk" true; then echo golden=free
else echo "golden=busy $(head -c 160 "$lk" | tr '\n' ' ')"; fi
pgrep -ax "$BUSY_RE" | sed 's/^/proc=/'
echo "governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"
echo "profile=$(cat /sys/firmware/acpi/platform_profile 2>/dev/null)"
for f in /sys/class/drm/card*/device/tile0/gt0/freq0; do
  [ -d "$f" ] && echo "gpufreq=xe $(cat $f/act_freq) $(cat $f/cur_freq) $(cat $f/max_freq) $(cat $f/rp0_freq)"
done
for c in /sys/class/drm/card?; do
  [ -e "$c/gt_RP0_freq_mhz" ] && echo "gpufreq=i915 $(cat $c/gt_act_freq_mhz) $(cat $c/gt_cur_freq_mhz) $(cat $c/gt_max_freq_mhz) $(cat $c/gt_RP0_freq_mhz)"
  [ -e "$c/device/power_dpm_force_performance_level" ] && echo "amdlevel=$(cat $c/device/power_dpm_force_performance_level) $(grep '\*' $c/device/pp_dpm_sclk 2>/dev/null | tr -s ' ')"
done
if [ -n "$EXE" ]; then
  d=$(dirname "$EXE")
  for i in 1 2 3 4 5 6; do
    if [ -f "$d/CMakeCache.txt" ]; then
      echo "cache=$d/CMakeCache.txt"
      grep -E '^(CMAKE_PROJECT_openpete_INCLUDE|CMAKE_C_FLAGS|CMAKE_CXX_FLAGS)[:=]' "$d/CMakeCache.txt" | sed 's/^/cacheline=/'
      break
    fi
    d=$(dirname "$d")
  done
fi
echo end=1
"""


def remote_readings(cfg, exe):
    """key -> [values] from the fleet host, or None when ssh fails."""
    script = "FLEET_ROOT=%s\nBUSY_RE=%s\nEXE=%s\n%s" % (
        cfg.get("fleet_root", "~/.cache/openpete-fleet").replace("~", "$HOME", 1),
        shlex.quote(LINUX_BUSY_RE), shlex.quote(exe or ""), REMOTE_SH)
    rc, out = _run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
                    cfg["ssh"], "bash -s"], timeout=60, input=script)
    if rc is None or "end=1" not in out:
        return None
    d = {}
    for line in out.splitlines():
        k, _, v = line.partition("=")
        d.setdefault(k, []).append(v)
    return d


def _one(d, k):
    return (d.get(k) or [""])[0].strip()


def remote_checks(host, cfg, exe):
    """The fleet host's checks."""
    d = remote_readings(cfg, exe)
    if d is None:
        return [Check("remote", "UNCHECKED", "ssh %s did not answer" % cfg.get("ssh"))]
    out = []
    g = _one(d, "golden")
    out.append(Check("golden-lock", "PASS" if g == "free" else "FAIL",
                     "free" if g == "free" else "held: " + g[5:]))
    procs = d.get("proc", [])
    out.append(Check("idle", "FAIL" if procs else "PASS",
                     "%d busy: %s" % (len(procs), "; ".join(p[:80] for p in procs[:4]))
                     if procs else "no game, emulator, compiler or profiler process"))
    try:
        hz = int(_one(d, "clk_tck"))
    except ValueError:
        hz = 100
    hot = hot_processes(proc_ticks("\n".join(d.get("pstat0", []))),
                        proc_ticks("\n".join(d.get("pstat1", []))), LOAD_SAMPLE_S, (), hz)
    out.append(load_check("load", _one(d, "stat0"), _one(d, "stat1"),
                          _one(d, "loadavg"), _one(d, "nproc"), hot))
    try:
        avail = int(_one(d, "MemAvailable"))
        gpua = _one(d, "GPUActive")
        floor = MEM_MIN_KB.get(host)
        reading = "MemAvailable %.1f GiB%s; GPUActive %s" % (
            avail / 1048576.0, " (floor %.0f GiB)" % (floor / 1048576.0) if floor else "",
            "%.1f GiB" % (int(gpua) / 1048576.0) if gpua else "not reported")
        st = "INFO" if floor is None else ("PASS" if avail >= floor else "FAIL")
        out.append(Check("mem", st, reading, required=floor is not None))
    except ValueError:
        out.append(Check("mem", "UNCHECKED", "/proc/meminfo unreadable",
                         required=host in MEM_MIN_KB))
    freqs = d.get("gpufreq", [])
    if freqs:
        kind, act, cur, mx, rp0 = (freqs[0].split() + ["?"] * 5)[:5]
        capped = mx.isdigit() and rp0.isdigit() and int(mx) < int(rp0)
        out.append(Check("gpu-clock", "FAIL" if capped else "PASS",
                         "%s act %s cur %s max %s RP0 %s MHz%s" % (
                             kind, act, cur, mx, rp0, " (max capped below RP0)" if capped else "")))
    elif d.get("amdlevel"):
        out.append(Check("gpu-clock", "INFO", "amdgpu " + _one(d, "amdlevel"), required=False))
    else:
        out.append(Check("gpu-clock", "UNCHECKED", "no xe/i915/amdgpu frequency files",
                         required=False))
    prof = _one(d, "profile")
    if prof:
        out.append(Check("power-profile", "FAIL" if prof in LOW_POWER_PROFILES else "PASS", prof))
    else:
        out.append(Check("power-profile", "UNCHECKED", "no ACPI platform_profile",
                         required=False))
    out.append(Check("governor", "INFO", _one(d, "governor") or "none", required=False))
    if exe:
        lines = d.get("cacheline", [])
        inc = next((l.split("=", 1)[1] for l in lines
                    if l.startswith("CMAKE_PROJECT_openpete_INCLUDE")), "")
        base = os.path.basename(inc)
        if not d.get("cache"):
            out.append(Check("tuned", "UNCHECKED", "no CMakeCache.txt above %s" % exe))
        elif any("-fsanitize" in l for l in lines):
            out.append(Check("tuned", "FAIL", "-fsanitize in " + _one(d, "cache")))
        elif re.match(r"(linux-release|windows-mingw).*\.cmake$", base):
            out.append(Check("tuned", "PASS", base[:-len(".cmake")]))
        else:
            out.append(Check("tuned", "FAIL", "preset include %r" % (base or "(none)")))
    else:
        out.append(Check("tuned", "UNCHECKED", "no exe named"))
    return out


# ---- assembly ----------------------------------------------------------------

class Decl:
    """What a leg declares: its metric class, --timer, --pixels, --staged,
    the monitor --baseline and the leg --record path. A Decl with cls None
    (bench_guard's harnesses) runs no class, pixel or neighbour check and
    requires the display awake on Windows."""

    def __init__(self, cls=None, timer=False, pixels=None, staged=(), baseline=None,
                 record=None):
        self.cls, self.timer, self.pixels = cls, timer, pixels
        self.staged, self.baseline, self.record = set(staged), baseline, record

    def as_dict(self):
        return {"class": self.cls, "timer": self.timer, "pixels": self.pixels,
                "staged": sorted(self.staged), "baseline": self.baseline}


def run_checks(target, exe=None, args=(), env=None, source_exe=None, decl=None):
    """Every check for `target`, as a list of Check; `decl` is the leg's Decl
    (None: the harness checks only)."""
    env = env or {}
    args = list(args)
    leg = decl is not None
    decl = decl or Decl()
    ds = None
    if target in ("local", "windows"):
        hosts = _fleet_hosts()
        out = [check_lock(None), check_tuned(exe, source_exe), check_idle_linux(),
               check_load_linux(), check_idle_windows(), check_load_windows(),
               check_golden_lock_local(hosts)]
        if target == "windows":
            ds = windows_display_state()
            need = display_need(decl.cls, decl.timer) if leg else "awake"
            out += [check_display(ds, need),
                    check_monitors(ds, decl.baseline, required=decl.cls == "cadence"),
                    check_windows_local(exe, args)]
    else:
        cfg = _fleet_hosts().get(target)
        if cfg is None or cfg.get("transport") != "ssh":
            return [Check("target", "UNCHECKED", "%s is not an ssh host in fleet-hosts.toml"
                          % target)]
        out = [check_lock(target)] + remote_checks(target, cfg, exe)
    out += [check_window(exe, args, env),
            Check("present-mode", "INFO", requested_present_mode(args, env), required=False)]
    if leg:
        out += [check_class(decl.cls, decl.timer, args, env, primary_hz(ds)),
                check_pixels(decl.cls, decl.timer, args, decl.pixels)]
        if target in ("local", "windows"):
            out.append(check_neighbours(exe, args, env, decl.staged))
        else:
            out.append(Check("neighbours", "UNCHECKED", "not read on a fleet host",
                             required=False))
    if decl.record:
        write_record(decl.record, {
            "written": _now_iso(), "target": target, "exe": exe, "args": args,
            "decl": decl.as_dict(), "monitors": (ds or {}).get("monitors"),
            "neighbours": exe_neighbours(exe, args, env) if exe and target in (
                "local", "windows") else None,
            "checks": [[c.name, c.status, c.reading] for c in out]})
    return out


def apply_accepts(checks, accepts):
    for c in checks:
        if c.name in accepts and c.status in ("FAIL", "UNCHECKED"):
            c.reading = "%s (was %s)" % (c.reading, c.status)
            c.status = "ACCEPTED"
    return checks


def report(target, checks, stream=sys.stdout):
    """Print one line per check and the verdict line; the blocking names."""
    for c in checks:
        stream.write("preflight[%s] %s: %s %s%s\n" % (
            target, c.name, c.status, c.reading, "" if c.required else " (not required)"))
    bad = [c.name for c in checks if c.blocks()]
    stream.write("preflight[%s]: %s\n" % (
        target, "REFUSED (%s)" % ", ".join(bad) if bad else "PASS"))
    stream.flush()
    return bad


def preflight(target, exe=None, args=(), env=None, source_exe=None, decl=None,
              accepts=(), wait=False):
    """Run and print the checks; with `wait`, re-read while only waitable
    checks block. Returns the list of blocking check names."""
    while True:
        checks = apply_accepts(run_checks(target, exe, args, env, source_exe, decl),
                               set(accepts))
        bad = report(target, checks)
        if not bad or not wait or not all(c.waitable() for c in checks if c.blocks()):
            return bad
        time.sleep(WAIT_POLL_S)


# ---- post: the finished leg's log and pace log --------------------------------

RENDER_RES_RE = re.compile(r"^\[render-res\] why=(\w+) .*?\bT=(\d+)x(\d+) .*?\bC=(\d+)x(\d+) ",
                           re.M)


def linux_path(p):
    """A Windows drive path (C:\\x\\y) as its WSL mount (/mnt/c/x/y); other
    paths unchanged."""
    m = re.match(r"^([A-Za-z]):[\\/](.*)$", p or "")
    if not m:
        return p
    return "/mnt/%s/%s" % (m.group(1).lower(), m.group(2).replace("\\", "/"))


def render_sizes(text):
    """[(why, (Tw, Th), (Cw, Ch))] from the log's [render-res] lines: T is the
    render target (SSAA included), C the window or capture size."""
    return [(m.group(1), (int(m.group(2)), int(m.group(3))), (int(m.group(4)), int(m.group(5))))
            for m in RENDER_RES_RE.finditer(text)]


def _wxh(t):
    return "%dx%d" % t


def post_size_checks(text, decl, expect_window):
    """window-actual (C of every live [render-res] line against the requested
    window) and pixels-actual (the last T against the declared pixels)."""
    out = []
    sizes = render_sizes(text)
    live = [z for z in sizes if z[0] == "live"] or sizes
    if expect_window:
        if not live:
            out.append(Check("window-actual", "UNCHECKED", "no [render-res] line"))
        else:
            cs = sorted({_wxh(c) for _, _, c in live})
            out.append(Check("window-actual", "PASS" if cs == [expect_window] else "FAIL",
                             "window %s, requested %s" % (",".join(cs), expect_window)))
    if not sizes:
        need = decl.pixels is not None or (decl.cls == "cost" and not decl.timer)
        out.append(Check("pixels-actual", "UNCHECKED", "no [render-res] line", need))
        return out
    _, t, c = sizes[-1]
    reading = "render target %s = %d px (window %s)" % (_wxh(t), t[0] * t[1], _wxh(c))
    if decl.pixels:
        ok = _wxh(t) == decl.pixels
        out.append(Check("pixels-actual", "PASS" if ok else "FAIL",
                         "%s; declared %s" % (reading, decl.pixels)))
    elif decl.cls == "cost" and not decl.timer:
        out.append(Check("pixels-actual", "PASS" if t == c else "FAIL",
                         "%s; declared native size, --ssaa 1" % reading))
    else:
        out.append(Check("pixels-actual", "INFO", reading, required=False))
    return out


def _pacelog():
    sys.path.insert(0, os.path.join(ROOT, "scripts", "pacing"))
    import pacelog
    return pacelog


def present_rate(log, window_s):
    """(presents per second, presents, repeats) over the last `window_s`
    seconds of live presents (not-live presents excluded; a REPEAT counts,
    since it fills a refresh), or None with fewer than two."""
    pl = _pacelog()
    pres = sorted((r.t_ns, r.flags) for r in log.records
                  if r.type == pl.PRESENT and not (r.flags & pl.F_NOT_LIVE))
    if len(pres) < 2:
        return None
    end = pres[-1][0]
    w = [p for p in pres if p[0] >= end - window_s * 1e9]
    if len(w) < 2:
        return None
    reps = sum(1 for _, f in w if (f & 3) == pl.REPEAT)
    return (len(w) - 1) / ((w[-1][0] - w[0][0]) / 1e9), len(w), reps


def post_pace_checks(pace_path, decl, static):
    """present-mode-actual, refresh-actual, and the class's rate check, from
    the pace log's env header and records."""
    presents = decl.cls == "cadence" or (decl.cls == "cost" and not decl.timer)
    if not pace_path:
        return [Check("pace-log", "UNCHECKED", "no pace log (--pace-log)", presents)]
    try:
        lg = _pacelog().read(pace_path)
    except (SystemExit, OSError) as e:
        return [Check("pace-log", "UNCHECKED", "%s unreadable: %s" % (pace_path, e), presents)]
    env = lg.env
    pm = env.get("present_mode", "?")
    want = {"cadence": "fifo", "cost": None if decl.timer else "immediate"}.get(decl.cls)
    out = [Check("present-mode-actual", "INFO" if not want else ("PASS" if pm == want else "FAIL"),
                 "%s%s" % (pm, "; class %s wants %s" % (decl.cls, want) if want else ""),
                 required=bool(want))]
    try:
        hz = int(env.get("refresh_mhz", "0")) / 1000.0
    except ValueError:
        hz = 0.0
    out.append(Check("refresh-actual", "INFO", "%.3f Hz, fullscreen=%s, render_fps=%s" % (
        hz, env.get("fullscreen", "?"), env.get("render_fps", "?")), required=False))
    rate = present_rate(lg, CADENCE_STEADY_S)
    if decl.cls == "cadence" and static:
        if rate is None or hz <= 0:
            out.append(Check("steady-rate", "UNCHECKED", "no live presents or no refresh_mhz"))
        else:
            ok = rate[0] >= CADENCE_MIN_SHARE * hz
            out.append(Check("steady-rate", "PASS" if ok else "FAIL",
                             "static scene %.1f presents/s over the last %.0f s (%d presents, "
                             "%d repeats); bound %.2f x %.3f Hz = %.1f" % (
                                 rate[0], CADENCE_STEADY_S, rate[1], rate[2],
                                 CADENCE_MIN_SHARE, hz, CADENCE_MIN_SHARE * hz)))
    elif decl.cls == "cost" and not decl.timer:
        try:
            cap = int(env.get("render_fps", "0"))
        except ValueError:
            cap = 0
        if rate is None or cap <= 0:
            out.append(Check("cap-headroom", "UNCHECKED", "no live presents or no render_fps"))
        else:
            ok = cap >= COST_MIN_CAP and rate[0] < COST_CAP_SHARE * cap
            out.append(Check("cap-headroom", "PASS" if ok else "FAIL",
                             "%.1f presents/s over the last %.0f s against the cap %d (bound: "
                             "cap >= %d and rate < %.1f x cap)" % (
                                 rate[0], CADENCE_STEADY_S, cap, COST_MIN_CAP, COST_CAP_SHARE)))
    return out


def post_checks(log, decl=None, expect_window=None, pace_log=None, static=False):
    """Checks read from a finished leg's log and pace log. The main exe
    prints no present-mode line under FIFO and no swapchain-extent line; the
    present mode, refresh and fullscreen come from the pace log's env header,
    the sizes from the [render-res] lines."""
    decl = decl or Decl()
    try:
        with open(log, errors="replace") as f:
            text = f.read()
    except OSError:
        return [Check("post-log", "UNCHECKED", "no log at %s" % log)]
    return post_size_checks(text, decl, expect_window) + post_pace_checks(
        pace_log, decl, static)


def _target_for(target, exe):
    if target:
        return target
    return "windows" if exe and exe.lower().endswith(".exe") else "local"


def _decl_args(p):
    p.add_argument("--class", dest="cls", choices=CLASSES, default=None,
                   help="the leg's metric class (cost | cadence | boot)")
    p.add_argument("--timer", action="store_true",
                   help="cost leg measured by a GPU/CPU timer, not by presents")
    p.add_argument("--pixels", default=None,
                   help="WxH render target the leg declares ([render-res] T=, SSAA included)")
    p.add_argument("--staged", default="",
                   help="comma list of texpack,config,mods the leg stages beside the exe")
    p.add_argument("--baseline", default=None,
                   help="leg record whose monitor set this leg must match (written when absent)")
    p.add_argument("--record", default=None, help="write this leg's record (JSON) here")
    p.add_argument("--pace-log", default=None,
                   help="the leg's pace log for post (default: the leg's own --pace-log)")
    p.add_argument("--static", action="store_true",
                   help="the scene is static: post checks the steady present rate")
    p.add_argument("--expect-window", default=None)
    p.add_argument("--log", default=None)


def _decl(a):
    staged = {x for x in a.staged.split(",") if x}
    bad = staged - set(STAGEABLE)
    if bad:
        raise SystemExit("preflight: --staged takes %s, got %s" % (",".join(STAGEABLE),
                                                                    ",".join(sorted(bad))))
    return Decl(a.cls, a.timer, a.pixels, staged, a.baseline, a.record)


def _append_post(record, checks):
    if not record:
        return
    try:
        with open(record) as f:
            rec = json.load(f)
    except (OSError, ValueError):
        rec = {}
    rec["post"] = [[c.name, c.status, c.reading] for c in checks]
    write_record(record, rec)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd_name", required=True)
    for name in ("check", "leg"):
        p = sub.add_parser(name)
        p.add_argument("--target", default=None, help="local | windows | <fleet host>")
        p.add_argument("--exe", default=None)
        p.add_argument("--source-exe", default=None,
                       help="the build-tree exe a Windows-local copy was made from")
        p.add_argument("--accept", action="append", default=[],
                       help="a check name whose FAIL/UNCHECKED is accepted (reported)")
        p.add_argument("--wait", action="store_true")
        _decl_args(p)
        p.add_argument("rest", nargs=argparse.REMAINDER)
    p = sub.add_parser("post")
    p.add_argument("--target", default="leg")
    p.add_argument("--accept", action="append", default=[])
    _decl_args(p)
    a = ap.parse_args(argv)
    decl = _decl(a)
    if a.cmd_name == "post":
        if not a.log:
            sys.stderr.write("preflight: post needs --log\n")
            return 64
        checks = apply_accepts(post_checks(a.log, decl, a.expect_window,
                                           linux_path(a.pace_log), a.static), set(a.accept))
        _append_post(a.record, checks)
        return EXIT_REFUSED if report(a.target, checks) else 0
    rest = a.rest[1:] if a.rest[:1] == ["--"] else a.rest
    exe, args, env = split_leg(rest)
    exe = a.exe or exe
    target = _target_for(a.target, exe)
    win = _flag(args, "--window")
    expect = a.expect_window or (win if parse_wxh(win) else None)
    bad = preflight(target, exe, args, env, a.source_exe, decl, a.accept, a.wait)
    if bad:
        return EXIT_REFUSED
    if a.cmd_name == "check":
        return 0
    if not rest:
        sys.stderr.write("preflight: leg needs -- CMD\n")
        return 64
    rc = subprocess.call(rest)
    if a.log:
        checks = apply_accepts(post_checks(a.log, decl, expect,
                                           linux_path(a.pace_log or _flag(args, "--pace-log")),
                                           a.static), set(a.accept))
        _append_post(a.record, checks)
        if report(target + " post", checks) and rc == 0:
            return EXIT_REFUSED
    return rc


if __name__ == "__main__":
    sys.exit(main())
