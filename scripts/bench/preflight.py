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
  display       Windows display awake: last input more recent than the
                display-off timeout (GetLastInputInfo, powercfg VIDEOIDLE);
                inferred, since Windows exposes no per-monitor power state
                without elevation; --no-display for legs that do not present
  refresh       the Windows display's refresh rate (INFO)
  present-mode  the present mode the leg asks for (INFO; `post` reads the log)
  window        the requested window size is honoured by the arguments:
                fullscreen with --background keeps the boot window, and
                fullscreen ignores --window
  windows-local the exe and every existing path argument live on a Windows
                drive (/mnt/<letter>/); a path on the Linux side is read over
                9P at Windows run time
  mem           MemAvailable (ptah: at least 8 GiB, its xe driver leaks
                GPUActive memory per process); GPUActive is INFO
  gpu-clock     GPU max frequency equals RP0 (no cap); current is INFO
  power-profile ACPI platform profile not low-power / quiet / cool
  governor      CPU frequency governor (INFO)

Commands:
  check [--target T] [--exe E] [-- leg args]     print the checks; exit 0/65
  leg   [--target T] [--log L] [--wait] -- CMD... preflight, run CMD, then the
        `post` checks on L when given; refuses (65) without running CMD
  post  --log L [--expect-window WxH]            read the leg's log: present
        mode and swapchain extent against the request

Exit: 0 every required check passed, 65 refused, 64 usage.
"""
import argparse
import csv
import fcntl
import hashlib
import importlib.util
import io
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
    return Check("window", "PASS", "windowed %s (post reads the swapchain extent)" % win)


def check_present_mode(args, env):
    """INFO: the present mode the leg requests."""
    vs = _flag(args, "--vsync")
    pm = env.get("OPENPETE_PRESENT_MODE", os.environ.get("OPENPETE_PRESENT_MODE"))
    if vs is not None:
        what = "--vsync %s" % (vs or "1")
    elif pm:
        what = "OPENPETE_PRESENT_MODE=%s" % pm
    else:
        what = "default (FIFO, vsync on)"
    return Check("present-mode", "INFO", what, required=False)


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


# One PowerShell call for the display state and the refresh rate.
DISPLAY_QUERY_PS = r"""
$ErrorActionPreference = 'Stop'
Add-Type -Namespace OP -Name In -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct LII { public uint cb; public uint t; }
[DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LII p);
'@
$l = New-Object OP.In+LII; $l.cb = 8
[void][OP.In]::GetLastInputInfo([ref]$l)
"idle_ms=" + ([uint32][Environment]::TickCount - $l.t)
$q = powercfg /q SCHEME_CURRENT SUB_VIDEO VIDEOIDLE | Out-String
if ($q -match 'Current AC Power Setting Index:\s*0x([0-9a-fA-F]+)') { "videoidle_ac=" + [Convert]::ToInt64($matches[1], 16) }
foreach ($c in Get-CimInstance Win32_VideoController) { "refresh=" + $c.Name + "|" + $c.CurrentRefreshRate + "|" + $c.CurrentHorizontalResolution + "x" + $c.CurrentVerticalResolution }
"""


def windows_display_state():
    """{"idle_ms", "videoidle_ac", "refresh": [...]} from Windows, or None."""
    rc, out = _run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                    DISPLAY_QUERY_PS], timeout=60)
    if rc != 0:
        return None
    d = {"refresh": []}
    for line in out.replace("\r", "").splitlines():
        k, _, v = line.partition("=")
        if k == "refresh":
            d["refresh"].append(v)
        elif k in ("idle_ms", "videoidle_ac"):
            try:
                d[k] = int(v)
            except ValueError:
                pass
    return d if "idle_ms" in d else None


def check_display(ds, required):
    if ds is None:
        return Check("display", "UNCHECKED", "powershell.exe query failed", required)
    idle = ds["idle_ms"] / 1000.0
    to = ds.get("videoidle_ac")
    if to is None:
        return Check("display", "UNCHECKED", "last input %.0f s ago; display-off timeout "
                     "unreadable" % idle, required)
    if to == 0:
        return Check("display", "PASS", "display-off timeout never; last input %.0f s ago"
                     % idle, required)
    st = "PASS" if idle < to else "FAIL"
    word = "awake" if idle < to else "asleep"
    return Check("display", st, "%s (inferred: last input %.0f s ago, display-off timeout "
                 "%d s); --hold-display wakes it" % (word, idle, to), required)


def check_refresh(ds):
    if ds is None or not ds["refresh"]:
        return Check("refresh", "UNCHECKED", "Win32_VideoController unreadable", required=False)
    parts = []
    for r in ds["refresh"]:
        name, hz, res = (r.split("|") + ["", "", ""])[:3]
        if hz:
            parts.append("%s %s Hz %s" % (name, hz, res))
    return Check("refresh", "INFO", "; ".join(parts) or "no active controller", required=False)


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

def run_checks(target, exe=None, args=(), env=None, source_exe=None, no_display=False):
    """Every check for `target`, as a list of Check."""
    env = env or {}
    args = list(args)
    if target in ("local", "windows"):
        hosts = _fleet_hosts()
        win = target == "windows"
        out = [check_lock(None), check_tuned(exe, source_exe), check_idle_linux(),
               check_load_linux(), check_idle_windows(), check_load_windows(),
               check_golden_lock_local(hosts)]
        if win:
            ds = windows_display_state()
            out += [check_display(ds, required=not no_display), check_refresh(ds),
                    check_windows_local(exe, args)]
        out += [check_window(exe, args, env), check_present_mode(args, env)]
        return out
    hosts = _fleet_hosts()
    cfg = hosts.get(target)
    if cfg is None or cfg.get("transport") != "ssh":
        return [Check("target", "UNCHECKED", "%s is not an ssh host in fleet-hosts.toml" % target)]
    out = [check_lock(target)] + remote_checks(target, cfg, exe)
    out += [check_window(exe, args, env), check_present_mode(args, env)]
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


def preflight(target, exe=None, args=(), env=None, source_exe=None, no_display=False,
              accepts=(), wait=False):
    """Run and print the checks; with `wait`, re-read while only waitable
    checks block. Returns the list of blocking check names."""
    while True:
        checks = apply_accepts(run_checks(target, exe, args, env, source_exe, no_display),
                               set(accepts))
        bad = report(target, checks)
        if not bad or not wait or not all(c.waitable() for c in checks if c.blocks()):
            return bad
        time.sleep(WAIT_POLL_S)


def post_checks(log, expect_window=None):
    """Checks read from a finished leg's log."""
    try:
        text = open(log, errors="replace").read()
    except OSError:
        return [Check("post-log", "UNCHECKED", "no log at %s" % log)]
    out = []
    m = re.findall(r"present mode: ([^\n]+)", text)
    out.append(Check("present-mode-actual", "INFO" if m else "UNCHECKED",
                     m[-1] if m else "no `present mode:` line", required=False))
    ext = re.findall(r"\(swapchain (\d+x\d+)\)", text)
    if not expect_window:
        out.append(Check("window-actual", "INFO" if ext else "UNCHECKED",
                         "swapchain " + ext[-1] if ext else "no swapchain extent line",
                         required=False))
    elif not ext:
        out.append(Check("window-actual", "UNCHECKED", "no swapchain extent line"))
    else:
        st = "PASS" if all(e == expect_window for e in ext) else "FAIL"
        out.append(Check("window-actual", st, "swapchain %s, requested %s"
                         % (",".join(sorted(set(ext))), expect_window)))
    hz = re.findall(r"refresh ([0-9.]+) Hz", text)
    if hz:
        out.append(Check("refresh-actual", "INFO", hz[-1] + " Hz", required=False))
    return out


def _target_for(target, exe):
    if target:
        return target
    return "windows" if exe and exe.lower().endswith(".exe") else "local"


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
        p.add_argument("--no-display", action="store_true",
                       help="the leg measures no presents (GPU timers, first tick)")
        p.add_argument("--wait", action="store_true")
        p.add_argument("--log", default=None)
        p.add_argument("--expect-window", default=None)
        p.add_argument("rest", nargs=argparse.REMAINDER)
    p = sub.add_parser("post")
    p.add_argument("--log", required=True)
    p.add_argument("--expect-window", default=None)
    p.add_argument("--target", default="leg")
    a = ap.parse_args(argv)
    if a.cmd_name == "post":
        return EXIT_REFUSED if report(a.target, post_checks(a.log, a.expect_window)) else 0
    rest = a.rest[1:] if a.rest[:1] == ["--"] else a.rest
    exe, args, env = split_leg(rest)
    exe = a.exe or exe
    target = _target_for(a.target, exe)
    win = _flag(args, "--window")
    expect = a.expect_window or (win if win and re.fullmatch(r"\d+x\d+", win) else None)
    bad = preflight(target, exe, args, env, a.source_exe, a.no_display, a.accept, a.wait)
    if bad:
        return EXIT_REFUSED
    if a.cmd_name == "check":
        return 0
    if not rest:
        sys.stderr.write("preflight: leg needs -- CMD\n")
        return 64
    rc = subprocess.call(rest)
    if a.log:
        report(target + " post", post_checks(a.log, expect))
    return rc


if __name__ == "__main__":
    sys.exit(main())
