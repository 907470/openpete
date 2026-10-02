#!/usr/bin/env python3
"""golden-fleet.py -- place presentation-golden units on a fleet of hosts.

    golden-fleet.py status [--hosts a,b]
    golden-fleet.py preflight [--hosts a,b] [--need-gib N] [--no-calibration]
    golden-fleet.py ship-image --hosts a,b
    golden-fleet.py add local [--dry-run] [--timings FILE]
    golden-fleet.py add <name> --ssh user@host [--engine podman|docker]
                    [--fleet-root DIR] [--dry-run] [--timings FILE]
    golden-fleet.py calibrate [--hosts a,b] [--dry-run]
    golden-fleet.py run [--game G | --all] [--tier T | --all-tiers]
                    [--hosts a,b | --no-local] [--pull divergent|all] [--wait]
    golden-fleet.py clean [--hosts a,b] [--tree PATH | --all]

Host file. $OPENPETE_FLEET_HOSTS, else `${XDG_CONFIG_HOME:-~/.config}/
openpete/fleet-hosts.toml`, the main checkout resolved through `git rev-parse
--git-common-dir` so a worktree resolves the same file. One table per host:

    [hosts.<name>]
    transport = "local" | "ssh"     # required; `local` is the coordinator
    ssh = "user@host"               # required for transport "ssh"
    fleet_root = "~/.cache/openpete-fleet"   # every fleet file on the host
    engine = "podman" | "docker"    # default: the first found on the host
    slots = N                       # CORPUS_JOBS on the host, measured by add
    speed = F                       # throughput relative to local (1.00)
    calib_s = N                     # calibration unit's wall time at slots (s)
    calibrated = "YYYY-MM-DD governor=G cpu=MODEL"
    enabled = true                  # false: never used
    default = true                  # in every run unless opted out
    build = true                    # may build the engine for a run
    nice = 19                       # CPU priority of units on the host

Only `add` and `calibrate` write the file: it replaces the named host's table and keeps
every other line. A missing host file makes status and preflight exit 64
naming the `add` command; a malformed one exits 1 naming the line or key.

status: per host, reachability (ssh with ConnectTimeout=5 and
keepalives), engine, the Id of openpete-linux-capture:fleet beside the
local image's Id, `<fleet_root>/image.id`, the golden lock (tried
non-blocking; the holder record the capture wrapper writes into it),
governor, calibration, free space and the size of fleet_root.

preflight: drops a host that is disabled, unreachable, without an engine,
uncalibrated, running a governor other than the calibrated one, short of
free space (--need-gib, plus the image size when the image must ship), or
whose golden lock is held; on the local host the slots are lowered to fit
2 GiB of MemAvailable per slot, and the host is dropped when none fits.
Each drop prints its reason. Exit 0 with a usable host, 64 with none.

ship-image: when a host's openpete-linux-capture:fleet Id differs from the
local image's, streams `<engine> save` into `ssh host <engine> load`, tags
the loaded Id :fleet and writes <fleet_root>/image.id. No label is added,
since a label changes the image config and so its Id.

add: measures a host with run's machinery (steps 1-3 below: preflight
without the calibration requirement, the host's clean and image ship, the
build on the local host and the positive sync set into
runs/<run-id>/tree on each measured host, and each leg's detached
supervisor, under the host golden lock on an ssh host and the coordinator's
on the local one). Each leg is the spyro-2 presentation unit (capture, then
--verdict-only) at one slots value, with LP_NUM_THREADS = ceil(cores /
slots) and MemAvailable sampled at 1 Hz on the host; a leg whose report
compares fewer than every blessed row or lists a missing tile ends the add.
The sweep runs slots 1, 2, 4, ... up to the host's core count and stops at
the first step improving under 10% or a leg whose minimum MemAvailable is
under 2 GiB. The rule: slots is the smallest sweep value past which
wall-clock improves by less than 10%, lowered while the minimum
MemAvailable during its leg is under 2 GiB. calib_s is the leg's wall time
at that slots. For an ssh host, speed = t_local / t_host, t_local being the
local host's calib_s; when the local host has none, a leg at its recorded
slots runs beside the sweep and its calib_s is written too. A local leg
counts only with this machine idle: the 1-minute load average at most
max(1, 0.1 x cores) before the leg (waited for up to 600 s) and, 90 s after
it ends, at most that bound plus a quarter of the load at its end; a
loaded leg is discarded and re-run, and the third loaded leg at one slots
value ends the add (64). `add local` records speed 1.00. The host's table
is then written with slots, speed, calib_s and calibrated (date, governor,
CPU model), and the add's wall time is printed.

--dry-run preflights and prints the build host and every planned leg, and
writes nothing. --timings FILE replaces the measurement with recorded legs
(JSON), ships no image and runs nothing:

    {"cores": 8,
     "sweep": [{"slots": 1, "wall_s": 1500, "min_memavail_kib": 9000000}, ...],
     "local": [{"slots": 4, "wall_s": 270, "load_before": 0.4,
                "load_end": 22.0, "load_after": 5.1}, ...]}

"local" is optional; without it t_local is the local host's calib_s.

calibrate: runs one leg at each host's recorded slots (--hosts, else every
enabled host; the local one under the idle rule) through the same
machinery, in parallel across hosts, and rewrites speed, calib_s and
calibrated; the governor is re-recorded, so a governor change is cleared by
calibrate. A leg under 2 GiB MemAvailable prints that `add` re-measures
slots. t_local is this calibration's local leg, else the local calib_s.

run: gates this checkout's working copy on the fleet and never blesses or
promotes. It detaches (setsid; the log and done marker are
<local fleet_root>/runs/<run-id>/coordinator.log and done, `rc=N` on the
first line of done); --wait follows it and exits with its code. The
coordinator:
  1. selects hosts (--hosts, else every enabled host with default = true,
     minus local under --no-local), preflights them, takes the local golden
     lock, runs clean on each host under its golden lock and ships the image;
  2. builds on the fastest host with build = true: in the checkout through
     the wrapper's `build` when that is the local host, else in the build
     host's snapshot, whose build tree then reaches the other hosts through
     this machine;
  3. syncs the positive set (git's tracked and non-ignored untracked files,
     submodules included; games/*/generated, the manifest globals and
     symbols, each run game's disc cue and bin, third_party/psxrecomp/build;
     the build tree minus home/) to <fleet_root>/runs/<run-id>/tree on every
     host with --link-dest from <fleet_root>/trees/<key>, then re-checks every
     copy with rsync -n --checksum over the same lists and refuses the run as
     torn on any change. While it reads the checkout it holds the checkout's
     build/linux-release-container.build-lock and runs a `golden-fleet-sync`
     marker process, which a build lock can test for; it then hashes the
     synced disc bytes against the blessed headers;
  4. places units longest first (ledger fleet-unit-times.tsv beside the
     store; a unit without a record is scaled from the spyro-1 presentation
     unit's 60 min by candidate size) on free hosts, one per host, each under
     a detached supervisor holding the host golden lock, with CORPUS_JOBS =
     slots, PRESENTATION_LEG_TIMEOUT = 300 / speed x 1.5 and the deadline
     expected time / speed x 2; a lost, dead or overdue attempt is killed
     (supervisor by PID and start time, containers by the label
     org.openpete.fleet=<run-id>) and re-queued once the golden lock is free,
     at most twice; a host unreachable for 10 min is dropped;
  5. runs a canary (one bk2 and one freecam level of the first unit's game)
     on every host first when two or more hosts capture, and refuses the run
     when a canary tile's digest differs between hosts;
  6. decides each unit on its capturing host (the harness's capture, then
     --verdict-only in the image), pulls verdict-report.txt, the capture
     record, capture-device.txt, credits-verdict.txt, the divergent tiles
     and both members of each identity pair over baseline (identity/) into
     the cleared local candidate directory (build/presentation-candidate and
     its per-game and dragon forms, or PRESENTATION_GOLDEN_OUT), refuses a
     unit whose report's blessed.tsv or identity-baseline hash or census
     differs from the local files, and renders heat diffs locally
     (presentation-corpus-diff.py --expect-subset);
  7. deletes runs/<run-id>/ on every host on every exit path (the local
     directory keeps its log and done marker).
Exit (and done marker rc): the worst unit in the order 1 > 64 > 0.
Negative-control knobs: OPENPETE_FLEET_AFTER_SYNC=<shell command> runs in
the checkout between the syncs and the recheck; OPENPETE_FLEET_POKE=
<host>:<job|unit|canary>:<tile substring> flips a pixel of the first
matching candidate tile on that host before its verdict.

clean: per host, under its golden lock (a busy host is skipped): removes
runs/ directories no process holds (active.lock) and with no done marker
newer than 24 h, signalling the dbus-daemon helpers their supervisors
recorded when PID and start time still match; exited fleet-labelled
containers; trees/<key> this coordinator wrote whose checkout path is gone
or unused for 14 days; images the fleet shipped (image.id and its history)
other than the current :fleet one and named by no other tag. --tree PATH
removes only this coordinator's trees of PATH. --all removes runs/, trees/,
the lock and image records, every fleet-labelled container and the :fleet
tag (the image itself only when no other tag names it), and fleet_root
itself when nothing else is in it.
"""

import argparse
import datetime
import fcntl
import glob
import hashlib
import importlib.util
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time

IMAGE_TAG = "openpete-linux-capture:fleet"
DEFAULT_FLEET_ROOT = "~/.cache/openpete-fleet"
CALIBRATION_GAME = "spyro-2"
GIB_KIB = 1024 * 1024
MEM_PER_SLOT_KIB = 2 * GIB_KIB
IMPROVEMENT_FLOOR = 0.10
IDLE_SETTLE_S = 90
IDLE_RETRIES = 3
SSH_OPTS = ["-o", "ConnectTimeout=5", "-o", "ServerAliveInterval=15",
            "-o", "ServerAliveCountMax=4", "-o", "BatchMode=yes"]
EXIT_SKIP = 64
CALIB_NEED_BYTES = 2 * 2**30  # calibration candidate set (194 MB) and store, with margin

GAMES = ("spyro-1", "spyro-2", "spyro-3")
TIER_GAMES = {"presentation": GAMES, "dragons": ("spyro-1",)}
# Candidate-set size per unit (MB, dev box); a unit with no recorded time gets
# the spyro-1 presentation unit's REF_UNIT_MIN scaled by size.
UNIT_MB = {("spyro-1", "presentation"): 1100, ("spyro-1", "dragons"): 1900,
           ("spyro-2", "presentation"): 194, ("spyro-3", "presentation"): 324}
REF_UNIT = ("spyro-1", "presentation")
REF_UNIT_MIN = 60.0
LEG_TIMEOUT_S = 300           # the harness's default leg timeout
CANARY_DEADLINE_S = 15 * 60
BUILD_DEADLINE_S = 90 * 60
BUILD_TREE = "build/linux-release-container"
GAME_TARGETS = tuple(f"openpete-{g.replace('-', '')}" for g in GAMES)
BUILD_TREE_BYTES = 1400 * 10**6
# One bk2 and one freecam level per game, run with --filter/--freecam-filter.
CANARY = {"spyro-1": ("alban-rescue", "gnastys_loot"),
          "spyro-2": ("shady-oasis-flame", "canyon_speedway"),
          "spyro-3": ("crystal-islands-flame", "molten_crater")}
CREDITS_NAME = "credits-verdict.txt"
LEDGER_COLS = ("date", "run", "game", "tier", "host", "speed", "wall_s", "norm_s", "cand_bytes")
POLL_S = 20
START_GRACE_S = 120
GRACE_S = 600
MAX_ATTEMPTS = 2
TREE_MAX_AGE_S = 14 * 86400
# Files of the coordinator's local run directory that outlive the run.
RUN_KEEP = ("coordinator.log", "done", "active.lock")

HOST_KEYS = {
    "transport": str, "ssh": str, "fleet_root": str, "engine": str,
    "slots": int, "speed": float, "calib_s": int, "calibrated": str, "enabled": bool,
    "default": bool, "build": bool, "nice": int,
}
KEY_ORDER = ["transport", "ssh", "fleet_root", "engine", "slots", "speed", "calib_s",
             "calibrated", "enabled", "default", "build", "nice"]


class FleetError(Exception):
    """An error that ends the command with a message and an exit code."""

    def __init__(self, msg, code=1):
        """Store the message and the exit code."""
        super().__init__(msg)
        self.code = code


# ---- host file --------------------------------------------------------------

def script_repo():
    """Return the root of the checkout or worktree holding this script."""
    here = os.path.dirname(os.path.abspath(__file__))
    return subprocess.run(["git", "-C", here, "rev-parse", "--show-toplevel"],
                          capture_output=True, text=True, check=True).stdout.strip()


def default_hosts_path():
    """Return ${XDG_CONFIG_HOME:-~/.config}/openpete/fleet-hosts.toml."""
    common = subprocess.run(
        ["git", "-C", script_repo(), "rev-parse", "--path-format=absolute",
         "--git-common-dir"], capture_output=True, text=True, check=True).stdout.strip()
    main = os.path.dirname(common.rstrip("/"))
    cfg = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.path.join(cfg, "openpete", "fleet-hosts.toml")


def hosts_path():
    """Return the host file path: $OPENPETE_FLEET_HOSTS, else the default."""
    return os.environ.get("OPENPETE_FLEET_HOSTS") or default_hosts_path()


def parse_value(raw, where):
    """Parse one TOML scalar (basic string, integer, float, boolean)."""
    if raw.startswith('"'):
        m = re.fullmatch(r'"((?:[^"\\]|\\.)*)"', raw)
        if not m:
            raise FleetError(f"{where}: unterminated or malformed string {raw}")
        return json.loads('"' + m.group(1) + '"')
    if raw in ("true", "false"):
        return raw == "true"
    if re.fullmatch(r"[+-]?\d+", raw):
        return int(raw)
    if re.fullmatch(r"[+-]?(\d+\.\d*|\.\d+|\d+)([eE][+-]?\d+)?", raw):
        return float(raw)
    raise FleetError(f"{where}: unsupported value {raw!r} (string, integer, float or boolean)")


def strip_comment(line):
    """Return the line without a trailing # comment outside a string."""
    in_str = esc = False
    for i, c in enumerate(line):
        if esc:
            esc = False
        elif c == "\\" and in_str:
            esc = True
        elif c == '"':
            in_str = not in_str
        elif c == "#" and not in_str:
            return line[:i]
    return line


def parse_toml(text, path):
    """Parse the flat host-file subset of TOML into {table: {key: value}}."""
    tables, cur = {}, None
    for n, line in enumerate(text.splitlines(), 1):
        body = strip_comment(line).strip()
        where = f"{path}:{n}"
        if not body:
            continue
        m = re.fullmatch(r"\[\s*([A-Za-z0-9_.-]+)\s*\]", body)
        if m:
            cur = m.group(1)
            if cur in tables:
                raise FleetError(f"{where}: table [{cur}] defined twice")
            tables[cur] = {}
            continue
        m = re.fullmatch(r"([A-Za-z0-9_-]+)\s*=\s*(.+)", body)
        if not m or cur is None:
            raise FleetError(f"{where}: expected [table] or key = value inside a table")
        key = m.group(1)
        if key in tables[cur]:
            raise FleetError(f"{where}: key {key} repeated in [{cur}]")
        tables[cur][key] = parse_value(m.group(2).strip(), where)
    return tables


def validate_hosts(tables, path):
    """Check the host tables against the schema and return {name: config}."""
    hosts = {}
    for tname, tbl in tables.items():
        if not tname.startswith("hosts.") or tname.count(".") != 1:
            raise FleetError(f"{path}: unexpected table [{tname}] (only [hosts.<name>])")
        name = tname.split(".", 1)[1]
        for key, val in tbl.items():
            want = HOST_KEYS.get(key)
            if want is None:
                raise FleetError(f"{path}: hosts.{name}.{key}: unknown key")
            ok = isinstance(val, want) and not (want is int and isinstance(val, bool))
            if want is float and isinstance(val, int) and not isinstance(val, bool):
                val, ok = float(val), True
            if not ok:
                raise FleetError(f"{path}: hosts.{name}.{key}: expected {want.__name__}, "
                                 f"got {type(val).__name__} {val!r}")
            tbl[key] = val
        transport = tbl.get("transport")
        if transport not in ("local", "ssh"):
            raise FleetError(f"{path}: hosts.{name}.transport: must be \"local\" or \"ssh\"")
        if transport == "ssh" and not tbl.get("ssh"):
            raise FleetError(f"{path}: hosts.{name}: transport \"ssh\" needs ssh = \"user@host\"")
        if transport == "local" and "ssh" in tbl:
            raise FleetError(f"{path}: hosts.{name}: transport \"local\" takes no ssh key")
        if tbl.get("engine") not in (None, "podman", "docker"):
            raise FleetError(f"{path}: hosts.{name}.engine: must be \"podman\" or \"docker\"")
        if tbl.get("slots", 1) < 1:
            raise FleetError(f"{path}: hosts.{name}.slots: must be at least 1")
        if tbl.get("speed", 1.0) <= 0:
            raise FleetError(f"{path}: hosts.{name}.speed: must be positive")
        hosts[name] = tbl
    locals_ = [n for n, t in hosts.items() if t["transport"] == "local"]
    if len(locals_) > 1:
        raise FleetError(f"{path}: more than one local host: {', '.join(locals_)}")
    return hosts


def load_hosts(required=True):
    """Load and validate the host file; absent and required exits 64 naming add."""
    path = hosts_path()
    if not os.path.exists(path):
        if required:
            raise FleetError(
                f"SKIP: no host file {path}; create it with "
                f"`scripts/golden-fleet.py add local` or `scripts/golden-fleet.py add "
                f"<name> --ssh user@host`", EXIT_SKIP)
        return path, {}
    with open(path, encoding="utf-8") as f:
        text = f.read()
    return path, validate_hosts(parse_toml(text, path), path)


def format_value(val):
    """Serialise one scalar as TOML."""
    if isinstance(val, bool):
        return "true" if val else "false"
    if isinstance(val, float):
        return f"{val:.2f}"
    if isinstance(val, int):
        return str(val)
    return json.dumps(val)


def format_table(name, cfg):
    """Serialise one host's table in KEY_ORDER."""
    lines = [f"[hosts.{name}]"]
    lines += [f"{k} = {format_value(cfg[k])}" for k in KEY_ORDER if k in cfg]
    return "\n".join(lines) + "\n"


def write_host_table(path, name, cfg):
    """Replace or append [hosts.<name>] in the host file, keeping other lines."""
    text = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
    lines = text.splitlines(keepends=True)
    header = re.compile(r"\s*\[\s*hosts\." + re.escape(name) + r"\s*\]")
    start = next((i for i, ln in enumerate(lines) if header.match(ln)), None)
    table = format_table(name, cfg)
    if start is None:
        sep = "" if not text or text.endswith("\n\n") else ("\n" if text.endswith("\n") else "\n\n")
        new = text + sep + table
    else:
        end = next((i for i in range(start + 1, len(lines))
                    if re.match(r"\s*\[", lines[i])), len(lines))
        tail = lines[end:]
        new = "".join(lines[:start]) + table + ("\n" if tail else "") + "".join(tail)
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(new)
    os.replace(tmp, path)
    validate_hosts(parse_toml(new, path), path)


# ---- transport --------------------------------------------------------------

class Host:
    """One fleet host: its config and a local or ssh transport."""

    def __init__(self, name, cfg):
        """Bind the host's name and validated config."""
        self.name = name
        self.cfg = cfg

    @property
    def local(self):
        """True for the coordinator's own machine."""
        return self.cfg["transport"] == "local"

    @property
    def fleet_root(self):
        """The configured fleet_root, unexpanded (`~` resolves on the host)."""
        return self.cfg.get("fleet_root", DEFAULT_FLEET_ROOT)

    def argv(self, remote_cmd):
        """Return the argv that runs a shell command string on the host."""
        if self.local:
            return ["bash", "-c", remote_cmd]
        return ["ssh", *SSH_OPTS, self.cfg["ssh"], remote_cmd]

    def run_script(self, script, args=(), timeout=120, check=False):
        """Run a bash script on the host via `bash -s`, script on stdin."""
        cmd = "bash -s -- " + " ".join(shlex.quote(str(a)) for a in args)
        try:
            cp = subprocess.run(self.argv(cmd), input=script, capture_output=True,
                                text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            raise FleetError(f"{self.name}: script timed out after {timeout} s")
        if check and cp.returncode != 0:
            raise FleetError(f"{self.name}: script exited {cp.returncode}: "
                             f"{cp.stderr.strip()[-400:]}")
        return cp

    def stream_command(self, argv):
        """Return the argv running `argv` on the host with stdin passed through."""
        return self.argv(" ".join(shlex.quote(a) for a in argv))


def parse_kv(text):
    """Parse key=value lines into a dict."""
    out = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


PROBE_SCRIPT = r'''
set -u
root="$1"; engine="$2"; image="$3"
case "$root" in "~") root="$HOME";; "~/"*) root="$HOME/${root#\~/}";; esac
echo "root=$root"
if [ -z "$engine" ]; then
    for e in podman docker; do command -v "$e" >/dev/null 2>&1 && { engine=$e; break; }; done
fi
echo "engine=$engine"
if [ -n "$engine" ] && command -v "$engine" >/dev/null 2>&1; then
    echo "engine_ok=1"
    echo "fleet_image_id=$("$engine" image inspect --format '{{.Id}}' "$image" 2>/dev/null)"
else
    echo "engine_ok=0"
fi
echo "image_id_file=$(cat "$root/image.id" 2>/dev/null)"
lock="$root/golden.lock"
if [ -e "$lock" ]; then
    if flock -n "$lock" true 2>/dev/null; then echo "lock=free"; else echo "lock=busy"; fi
    echo "lock_holder=$(head -c 200 "$lock" 2>/dev/null | tr '\n' ' ')"
else
    echo "lock=free"
    echo "lock_holder="
fi
echo "governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo none)"
echo "kill_user_processes=$(busctl get-property org.freedesktop.login1 /org/freedesktop/login1 \
    org.freedesktop.login1.Manager KillUserProcesses 2>/dev/null | awk '{print $2}')"
echo "linger=$(loginctl show-user "$(id -un)" -p Linger --value 2>/dev/null)"
echo "cpu=$(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //')"
echo "nproc=$(nproc)"
echo "memavail_kib=$(awk '/^MemAvailable:/{print $2}' /proc/meminfo)"
echo "loadavg1=$(cut -d' ' -f1 /proc/loadavg)"
d="$root"; while [ ! -e "$d" ]; do d="$(dirname "$d")"; done
echo "free_bytes=$(df -PB1 "$d" | awk 'NR==2{print $4}')"
if [ -d "$root" ]; then echo "root_bytes=$(du -sb "$root" 2>/dev/null | cut -f1)"; else echo "root_bytes=0"; fi
'''


def probe(host):
    """Probe a host in one script; returns a dict with reachable=False on failure."""
    try:
        cp = host.run_script(PROBE_SCRIPT, [host.fleet_root, host.cfg.get("engine", ""),
                                            IMAGE_TAG], timeout=60)
    except FleetError as e:
        return {"reachable": False, "error": str(e)}
    if cp.returncode != 0 or "root=" not in cp.stdout:
        return {"reachable": False,
                "error": (cp.stderr.strip() or f"exit {cp.returncode}")[-300:]}
    info = parse_kv(cp.stdout)
    info["reachable"] = True
    return info


def local_engine():
    """Return the coordinator's container engine (podman first), or None."""
    for e in ("podman", "docker"):
        if subprocess.run(["bash", "-c", f"command -v {e}"], capture_output=True).returncode == 0:
            return e
    return None


def local_image():
    """Return the image the coordinator ships: $OPENPETE_CAPTURE_IMAGE or the default."""
    return os.environ.get("OPENPETE_CAPTURE_IMAGE", "openpete-linux-capture")


def image_inspect(engine, image, fmt):
    """Return one inspect field of a local image, or '' when absent."""
    cp = subprocess.run([engine, "image", "inspect", "--format", fmt, image],
                        capture_output=True, text=True)
    return cp.stdout.strip() if cp.returncode == 0 else ""


def norm_id(image_id):
    """Strip a sha256: prefix so podman and docker Ids compare."""
    return (image_id or "").removeprefix("sha256:")


def calibrated_governor(cfg):
    """Return the governor recorded in `calibrated`, or None."""
    m = re.search(r"governor=(\S+)", cfg.get("calibrated", ""))
    return m.group(1) if m else None


def select_hosts(hosts, names):
    """Return Host objects for a comma-separated name list, or every host."""
    if not names:
        return [Host(n, c) for n, c in hosts.items()]
    out = []
    for n in names.split(","):
        if n not in hosts:
            raise FleetError(f"no host {n!r} in the host file (have: {', '.join(hosts)})")
        out.append(Host(n, hosts[n]))
    return out


# ---- locks ------------------------------------------------------------------

def expand_root(root):
    """Expand `~` in a local fleet_root."""
    return os.path.expanduser(root)


def try_golden_lock(root):
    """Take <root>/golden.lock non-blocking and write the holder record; fd or None."""
    root = expand_root(root)
    os.makedirs(root, exist_ok=True)
    lock = os.path.join(root, "golden.lock")
    fd = os.open(lock, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o644)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        os.close(fd)
        return None
    os.ftruncate(fd, 0)
    stamp = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
    os.write(fd, f"pid {os.getpid()} host {os.uname().nodename} golden-fleet "
                 f"since {stamp}\n".encode())
    return fd


# ---- preflight --------------------------------------------------------------

def preflight(hosts, need_bytes=0, require_calibration=True, info=None):
    """Check each host; return [(host, info, reasons, slots)] with reasons empty when usable."""
    engine = local_engine()
    local_id = norm_id(image_inspect(engine, local_image(), "{{.Id}}")) if engine else ""
    image_bytes = int(image_inspect(engine, local_image(), "{{.Size}}") or 0) if engine else 0
    results = []
    for h in hosts:
        reasons, slots = [], h.cfg.get("slots", 1)
        i = (info or {}).get(h.name) or probe(h)
        if not h.cfg.get("enabled", True):
            reasons.append("disabled (enabled = false)")
        if not i["reachable"]:
            reasons.append(f"unreachable: {i['error']}")
            results.append((h, i, reasons, 0))
            continue
        if i.get("engine_ok") != "1":
            reasons.append(f"no container engine ({i.get('engine') or 'none found'})")
        gov = calibrated_governor(h.cfg)
        if gov is None:
            if require_calibration:
                reasons.append("not calibrated (no governor= in calibrated); run add")
        elif gov != i["governor"]:
            reasons.append(f"governor {i['governor']}, calibrated at {gov}")
        need = need_bytes
        ships = norm_id(i.get("fleet_image_id")) != local_id and not h.local
        if ships:
            need += image_bytes
        free = int(i.get("free_bytes") or 0)
        if free < need:
            reasons.append(f"free space {free / 2**30:.1f} GiB under {i['root']}, "
                           f"need {need / 2**30:.1f} GiB"
                           + (" (image ships)" if ships else ""))
        if h.local:
            fit = int(i.get("memavail_kib") or 0) // MEM_PER_SLOT_KIB
            if fit < 1:
                reasons.append(f"MemAvailable {int(i['memavail_kib']) / GIB_KIB:.1f} GiB "
                               f"under 2 GiB for one slot")
            elif fit < slots:
                print(f"[fleet] {h.name}: slots lowered {slots} -> {fit} "
                      f"(MemAvailable {int(i['memavail_kib']) / GIB_KIB:.1f} GiB, 2 GiB per slot)")
                slots = fit
        if not h.local and i.get("kill_user_processes") == "true" and i.get("linger") != "yes":
            reasons.append("logind kills an ssh session's processes at logout "
                           "(KillUserProcesses=yes) and lingering is off; run "
                           "`loginctl enable-linger` on the host")
        if i.get("lock") == "busy":
            reasons.append(f"golden lock held: {i.get('lock_holder', '').strip()}")
        results.append((h, i, reasons, slots))
    return results


# ---- image shipping ---------------------------------------------------------

WRITE_IMAGE_ID = r'''
set -eu
root="$1"; engine="$2"; id="$3"; tag="$4"
case "$root" in "~") root="$HOME";; "~/"*) root="$HOME/${root#\~/}";; esac
"$engine" tag "$id" "$tag"
got="$("$engine" image inspect --format '{{.Id}}' "$tag")"
[ "${got#sha256:}" = "${id#sha256:}" ] || { echo "tag $tag names $got, not $id" >&2; exit 1; }
mkdir -p "$root"
# The Id a previous ship recorded joins image.id.history, from which clean
# removes images the fleet shipped and no longer uses.
old="$(cat "$root/image.id" 2>/dev/null || true)"
[ -n "$old" ] && [ "${old#sha256:}" != "${id#sha256:}" ] && printf '%s\n' "$old" >>"$root/image.id.history"
printf '%s\n' "$id" >"$root/image.id.tmp"
mv "$root/image.id.tmp" "$root/image.id"
'''


# The names a load may create beside :fleet (the archive's own tags, or the
# engine's default name); each that did not exist before the load is untagged.
LOAD_NAMES = ("openpete-linux-capture:latest", "localhost/openpete-linux-capture:latest")
NAMES_PRESENT = r'''
engine="$1"; shift
for n in "$@"; do "$engine" image inspect "$n" >/dev/null 2>&1 && echo "$n"; done
exit 0
'''


def transfer_image(host, engine, host_engine, local_id, dry_run):
    """Stream the image saved under the :fleet tag alone into the host's engine."""
    subprocess.run([engine, "tag", local_id, IMAGE_TAG], check=True, capture_output=True)
    save = [engine, "save", IMAGE_TAG]
    load = host.stream_command([host_engine, "load"])
    print(f"[fleet] {host.name}: {shlex.join(save)} | {shlex.join(load)}")
    if dry_run:
        return
    before = set(host.run_script(NAMES_PRESENT, [host_engine, *LOAD_NAMES]).stdout.split())
    t0 = time.monotonic()
    src = subprocess.Popen(save, stdout=subprocess.PIPE)
    dst = subprocess.run(load, stdin=src.stdout, capture_output=True, text=True)
    src.stdout.close()
    if src.wait() != 0 or dst.returncode != 0:
        raise FleetError(f"{host.name}: image transfer failed (save {src.returncode}, "
                         f"load {dst.returncode}): {dst.stderr.strip()[-300:]}")
    print(f"[fleet] {host.name}: shipped in {time.monotonic() - t0:.0f} s")
    after = set(host.run_script(NAMES_PRESENT, [host_engine, *LOAD_NAMES]).stdout.split())
    for name in sorted(after - before):
        host.run_script('"$1" rmi "$2" >/dev/null', [host_engine, name], check=True)
        print(f"[fleet] {host.name}: untagged {name}, which the load created")


def ship_image(host, info, dry_run=False):
    """Deliver the local image to the host as :fleet and record image.id; True when shipped."""
    engine = local_engine()
    if not engine:
        raise FleetError("no local container engine", EXIT_SKIP)
    local_id = norm_id(image_inspect(engine, local_image(), "{{.Id}}"))
    if not local_id:
        raise FleetError(f"no local image {local_image()}", EXIT_SKIP)
    host_engine = info.get("engine") or "podman"
    same = norm_id(info.get("fleet_image_id")) == local_id
    recorded = norm_id(info.get("image_id_file")) == local_id
    if same and recorded:
        print(f"[fleet] {host.name}: {IMAGE_TAG} is {local_id[:12]}; nothing to ship")
        return False
    if not same and not host.local:
        transfer_image(host, engine, host_engine, local_id, dry_run)
    print(f"[fleet] {host.name}: tag {local_id[:12]} {IMAGE_TAG}; write image.id")
    if not dry_run:
        host.run_script(WRITE_IMAGE_ID, [host.fleet_root, host_engine, local_id, IMAGE_TAG],
                        check=True)
    return True


# ---- calibration sweep ------------------------------------------------------

def sweep_values(cores):
    """Return the sweep's slot values: 1, 2, 4, ... up to cores, plus cores itself."""
    vals, s = [], 1
    while s <= cores:
        vals.append(s)
        s *= 2
    if vals[-1] != cores:
        vals.append(cores)
    return vals


def sweep_should_stop(legs):
    """True once the last leg improved on the one before by under 10% or breached memory."""
    if legs and legs[-1]["min_memavail_kib"] < MEM_PER_SLOT_KIB:
        return True
    return len(legs) >= 2 and legs[-1]["wall_s"] > (1 - IMPROVEMENT_FLOOR) * legs[-2]["wall_s"]


def pick_slots(legs):
    """Apply the slots rule to sweep legs; return (slots, wall_s at slots, explanation lines)."""
    legs = sorted(legs, key=lambda x: x["slots"])
    if not legs:
        raise FleetError("slots rule: no sweep legs")
    why, idx = [], len(legs) - 1
    for i in range(len(legs) - 1):
        a, b = legs[i], legs[i + 1]
        gain = 1 - b["wall_s"] / a["wall_s"]
        why.append(f"slots {a['slots']} -> {b['slots']}: {a['wall_s']:.0f} s -> "
                   f"{b['wall_s']:.0f} s, improvement {gain * 100:.1f}%")
        if gain < IMPROVEMENT_FLOOR:
            idx = i
            why.append(f"improvement under {IMPROVEMENT_FLOOR * 100:.0f}%: slots {a['slots']}")
            break
    else:
        why.append(f"every step improved by {IMPROVEMENT_FLOOR * 100:.0f}% or more: "
                   f"slots {legs[idx]['slots']}")
    while idx > 0 and legs[idx]["min_memavail_kib"] < MEM_PER_SLOT_KIB:
        why.append(f"slots {legs[idx]['slots']}: min MemAvailable "
                   f"{legs[idx]['min_memavail_kib'] / GIB_KIB:.2f} GiB under 2 GiB; lowered")
        idx -= 1
    if legs[idx]["min_memavail_kib"] < MEM_PER_SLOT_KIB:
        raise FleetError(f"slots rule: min MemAvailable under 2 GiB even at slots "
                         f"{legs[idx]['slots']}")
    return legs[idx]["slots"], legs[idx]["wall_s"], why


def idle_bound(cores):
    """Return the 1-minute load average at or under which the machine counts as idle."""
    return max(1.0, 0.1 * cores)


def leg_is_idle(leg, cores):
    """Apply the idle rule to one local leg; return (idle, explanation)."""
    bound = idle_bound(cores)
    after_bound = bound + 0.25 * leg["load_end"]
    ok = leg["load_before"] <= bound and leg["load_after"] <= after_bound
    return ok, (f"load before {leg['load_before']:.2f} (<= {bound:.2f}), "
                f"{IDLE_SETTLE_S} s after {leg['load_after']:.2f} (<= {after_bound:.2f})")


def pick_t_local(legs, cores):
    """Return the first idle local leg's wall time, printing each discarded leg."""
    for n, leg in enumerate(legs, 1):
        ok, why = leg_is_idle(leg, cores)
        print(f"[fleet] local leg {n} at slots {leg['slots']}: {leg['wall_s']:.0f} s; {why}; "
              + ("accepted" if ok else "loaded, discarded"))
        if ok:
            return leg["wall_s"]
    raise FleetError(f"no idle local leg in {len(legs)} attempts")


def loadavg1():
    """Return the local 1-minute load average."""
    return os.getloadavg()[0]


# ---- commands ---------------------------------------------------------------

def fmt_bytes(n):
    """Format a byte count in GiB."""
    return f"{int(n or 0) / 2**30:.1f} GiB"


def cmd_status(a):
    """Print each host's reachability, image, lock, governor, calibration and space."""
    path, hosts = load_hosts()
    engine = local_engine()
    local_id = norm_id(image_inspect(engine, local_image(), "{{.Id}}")) if engine else ""
    print(f"host file: {path}")
    print(f"local image: {local_image()} {local_id[:12] or 'absent'}")
    for h in select_hosts(hosts, a.hosts):
        i = probe(h)
        where = "local" if h.local else h.cfg["ssh"]
        print(f"[{h.name}] {where}")
        if not i["reachable"]:
            print(f"  reachable: no ({i['error']})")
            continue
        fid = norm_id(i.get("fleet_image_id"))
        rec = norm_id(i.get("image_id_file"))
        print("  reachable: yes")
        print(f"  engine: {i.get('engine') or 'none'}"
              + ("" if i.get("engine_ok") == "1" else " (missing)"))
        print(f"  {IMAGE_TAG}: {fid[:12] or 'absent'} "
              + ("(equals local)" if fid and fid == local_id else "(differs from local)"))
        print(f"  image.id: {rec[:12] or 'absent'}"
              + ("" if not rec else (" (equals :fleet)" if rec == fid else " (differs from :fleet)")))
        holder = i.get("lock_holder", "").strip()
        print(f"  golden lock: {i.get('lock')}" + (f"; holder record: {holder}" if holder else ""))
        gov = calibrated_governor(h.cfg)
        print(f"  governor: {i.get('governor')}"
              + ("" if gov is None else (" (matches calibration)" if gov == i.get("governor")
                                         else f" (calibrated at {gov})")))
        print(f"  calibrated: {h.cfg.get('calibrated', 'never')}; slots "
              f"{h.cfg.get('slots', '-')}, speed {h.cfg.get('speed', '-')}")
        print(f"  cpu: {i.get('cpu')} x{i.get('nproc')}; MemAvailable "
              f"{int(i.get('memavail_kib') or 0) / GIB_KIB:.1f} GiB; load {i.get('loadavg1')}")
        print(f"  free under {i['root']}: {fmt_bytes(i.get('free_bytes'))}; "
              f"fleet_root size {fmt_bytes(i.get('root_bytes'))}")
    return 0


def print_preflight(results):
    """Print preflight verdicts; return the usable (host, slots) pairs."""
    usable = []
    for h, _i, reasons, slots in results:
        if reasons:
            for r in reasons:
                print(f"[fleet] {h.name}: dropped: {r}")
        else:
            print(f"[fleet] {h.name}: usable, slots {slots}")
            usable.append((h, slots))
    return usable


def cmd_preflight(a):
    """Run preflight on the named hosts; exit 64 when none is usable."""
    _path, hosts = load_hosts()
    sel = [h for h in select_hosts(hosts, a.hosts)]
    usable = print_preflight(preflight(sel, int(a.need_gib * 2**30),
                                       require_calibration=not a.no_calibration))
    if not usable:
        print("[fleet] SKIP: no usable host")
        return EXIT_SKIP
    return 0


def cmd_ship_image(a):
    """Ship the local capture image to each named host whose :fleet Id differs."""
    _path, hosts = load_hosts()
    for h in select_hosts(hosts, a.hosts):
        i = probe(h)
        if not i["reachable"]:
            raise FleetError(f"{h.name}: unreachable: {i['error']}")
        ship_image(h, i, dry_run=a.dry_run)
    return 0


def new_host_cfg(a, existing):
    """Return the host's table before measurement: existing values under the CLI's."""
    cfg = dict(existing or {})
    cfg.pop("calibrated", None)  # the measurement records the governor it runs under
    if a.name == "local":
        cfg["transport"] = "local"
        cfg.pop("ssh", None)
        cfg.setdefault("nice", 19)
    else:
        if not a.ssh:
            raise FleetError("add <name> needs --ssh user@host (only `local` takes none)", 2)
        cfg["transport"] = "ssh"
        cfg["ssh"] = a.ssh
        cfg["engine"] = a.engine or cfg.get("engine", "podman")
    if a.fleet_root:
        cfg["fleet_root"] = a.fleet_root
    cfg.setdefault("fleet_root", DEFAULT_FLEET_ROOT)
    for k in ("enabled", "default", "build"):
        cfg.setdefault(k, True)
    return cfg


def calibrated_stamp(info):
    """Return the `calibrated` value: date, governor and CPU model of a probe."""
    return f"{datetime.date.today().isoformat()} governor={info['governor']} cpu={info['cpu']}"


def find_local(hosts):
    """Return the host file's local Host, or None."""
    return next((Host(n, c) for n, c in hosts.items() if c["transport"] == "local"), None)


def add_from_timings(a, path, hosts, host, cfg):
    """Apply the slots rule and speed to recorded legs (--timings); write the table unless --dry-run."""
    timings = json.load(open(a.timings))
    (_h, info, reasons, _s), = preflight([host], need_bytes=CALIB_NEED_BYTES,
                                         require_calibration=False)
    reasons = [r for r in reasons if not r.startswith("golden lock held")]
    for r in reasons:
        print(f"[fleet] {host.name}: preflight: {r}")
    if reasons:
        raise FleetError(f"{host.name}: preflight failed", EXIT_SKIP)
    cores = int(timings.get("cores") or info["nproc"])
    print(f"[fleet] legs from {a.timings} (cores {cores}); sweep values {sweep_values(cores)}")
    slots, t_host, why = pick_slots(timings["sweep"])
    for w in why:
        print(f"[fleet] rule: {w}")
    speed = 1.0
    if not host.local:
        local = find_local(hosts)
        if timings.get("local"):
            t_local = pick_t_local(timings["local"], os.cpu_count())
        elif local and local.cfg.get("calib_s"):
            t_local = float(local.cfg["calib_s"])
            print(f"[fleet] t_local {t_local:.0f} s: calib_s of [hosts.{local.name}]")
        else:
            raise FleetError("no local leg in --timings and no calib_s on a local host; "
                             "run `add local` first")
        speed = t_local / t_host
        print(f"[fleet] speed = t_local {t_local:.0f} s / t_host {t_host:.0f} s = {speed:.2f}")
    cfg.update(slots=slots, speed=round(speed, 2), calib_s=int(round(t_host)),
               calibrated=calibrated_stamp(info))
    return write_or_show(path, host.name, cfg, a.dry_run)


def write_or_show(path, name, cfg, dry_run):
    """Print a host table and write it into the host file unless dry_run."""
    print(f"[fleet] {'would write' if dry_run else 'writing'} {path}:")
    sys.stdout.write(format_table(name, cfg))
    if not dry_run:
        write_host_table(path, name, cfg)
    return 0


def cmd_add(a):
    """Measure a host through `run`'s machinery and record its slots, speed, calib_s and calibration."""
    t0 = time.monotonic()
    path, hosts = load_hosts(required=False)
    if a.name != "local" and hosts.get(a.name, {}).get("transport") == "local":
        raise FleetError(f"{a.name} is the local host; use `add local`")
    if a.name == "local":
        other = [n for n, c in hosts.items() if c["transport"] == "local" and n != "local"]
        if other:
            raise FleetError(f"the host file already names a local host: {other[0]}")
    cfg = new_host_cfg(a, hosts.get(a.name))
    host = Host(a.name, cfg)
    if a.timings:
        return add_from_timings(a, path, hosts, host, cfg)

    local = host if host.local else find_local(hosts)
    if local is None or (not host.local and not local.cfg.get("slots")):
        raise FleetError("no local host with recorded slots in the host file; run "
                         "`add local` first", EXIT_SKIP)
    plans, measure = {host.name: "sweep"}, [host]
    if not host.local and not local.cfg.get("calib_s"):
        plans[local.name] = local.cfg["slots"]
        measure.append(local)
        print(f"[fleet] {local.name} has no calib_s: t_local is measured in this add at "
              f"slots {local.cfg['slots']}")
    rc, co = run_calibration(local, measure, plans, a.dry_run)
    if rc != 0 or a.dry_run:
        return rc

    slots, t_host, why = pick_slots(co.legs[host.name])
    for w in why:
        print(f"[fleet] rule: {w}")
    speed = 1.0
    if not host.local:
        if local.name in plans:
            t_local = co.legs[local.name][-1]["wall_s"]
            lcfg = dict(local.cfg, calib_s=int(round(t_local)))
            write_or_show(path, local.name, lcfg, False)
        else:
            t_local = float(local.cfg["calib_s"])
            print(f"[fleet] t_local {t_local:.0f} s: calib_s of [hosts.{local.name}]")
        speed = t_local / t_host
        print(f"[fleet] speed = t_local {t_local:.0f} s / t_host {t_host:.0f} s = {speed:.2f}")
    cfg.update(slots=slots, speed=round(speed, 2), calib_s=int(round(t_host)),
               calibrated=calibrated_stamp(co.infos[host.name]))
    write_or_show(path, host.name, cfg, False)
    print(f"[fleet] add {host.name}: wall time {time.monotonic() - t0:.0f} s")
    return 0


def cmd_calibrate(a):
    """Re-run the calibration unit at each host's recorded slots; rewrite speed, calib_s and calibrated."""
    t0 = time.monotonic()
    path, hosts = load_hosts()
    names = a.hosts.split(",") if a.hosts else [
        n for n, c in hosts.items() if c.get("enabled", True)]
    for n in names:
        if n not in hosts:
            raise FleetError(f"no host {n!r} in the host file (have: {', '.join(hosts)})")
        if not hosts[n].get("slots"):
            raise FleetError(f"{n}: no recorded slots; run `add {n}` first", EXIT_SKIP)
    # The governor recorded in `calibrated` is what calibrate re-records, so
    # preflight runs without it.
    strip = {n: {k: v for k, v in c.items() if k != "calibrated"} for n, c in hosts.items()}
    local = find_local(strip)
    if local is None:
        raise FleetError("no local host in the host file; run `add local` first", EXIT_SKIP)
    measure = [local if n == local.name else Host(n, strip[n]) for n in names]
    plans = {h.name: h.cfg["slots"] for h in measure}
    rc, co = run_calibration(local, measure, plans, a.dry_run)
    if rc != 0 or a.dry_run:
        return rc
    if local.name in plans:
        t_local = co.legs[local.name][-1]["wall_s"]
    elif local.cfg.get("calib_s"):
        t_local = float(local.cfg["calib_s"])
    else:
        raise FleetError(f"{local.name} has no calib_s and was not calibrated; include it")
    for h in measure:
        leg = co.legs[h.name][-1]
        if leg["min_memavail_kib"] < MEM_PER_SLOT_KIB:
            print(f"[fleet] {h.name}: min MemAvailable {leg['min_memavail_kib'] / GIB_KIB:.2f} "
                  f"GiB under 2 GiB at slots {leg['slots']}; `add {h.name}` re-measures slots")
        speed = 1.0 if h.local else t_local / leg["wall_s"]
        print(f"[fleet] {h.name}: slots {leg['slots']}, {leg['wall_s']:.0f} s, speed {speed:.2f}")
        cfg = dict(hosts[h.name], speed=round(speed, 2), calib_s=int(round(leg["wall_s"])),
                   calibrated=calibrated_stamp(co.infos[h.name]))
        write_or_show(path, h.name, cfg, False)
    print(f"[fleet] calibrate: wall time {time.monotonic() - t0:.0f} s")
    return 0


# ---- host-side shell helpers ------------------------------------------------

# starttime PID prints field 22 of /proc/PID/stat (the process start time in
# clock ticks since boot), read after the comm field, which may hold spaces.
SH_STARTTIME = r'''
starttime() {
    local s
    s="$(cat "/proc/$1/stat" 2>/dev/null)" || return 1
    s="${s##*) }"
    set -- $s
    echo "${20}"
}
'''

# kill_helpers RUNDIR signals each helper recorded in RUNDIR/helpers.tsv as
# attributed whose comm is still dbus-daemon and whose start time still equals
# the recorded one; a reused PID never matches both.
SH_KILL_HELPERS = SH_STARTTIME + r'''
kill_helpers() {
    local f="$1/helpers.tsv" pid start flag
    [ -f "$f" ] || return 0
    while read -r pid start flag; do
        [ "$flag" = attributed ] || continue
        [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = dbus-daemon ] || continue
        [ "$(starttime "$pid")" = "$start" ] || continue
        kill -TERM "$pid" 2>/dev/null && echo "signalled helper dbus-daemon $pid (start $start)"
    done <"$f"
}
'''

# ---- clean ------------------------------------------------------------------

TREE_LIST_SCRIPT = r'''
set -u
root="$1"
case "$root" in "~") root="$HOME";; "~/"*) root="$HOME/${root#\~/}";; esac
for f in "$root"/trees/*.src; do
    [ -f "$f" ] || continue
    k="$(basename "$f" .src)"
    printf 'key=%s\t%s\n' "$k" "$(tr '\n' '\t' <"$f")"
done
'''

CLEAN_SCRIPT = SH_KILL_HELPERS + r'''
set -u
root="$1"; engine="$2"; mode="$3"; held="$4"; tag="$5"; shift 5
case "$root" in "~") root="$HOME";; "~/"*) root="$HOME/${root#\~/}";; esac
[ -d "$root" ] || { echo "no $root; nothing to clean"; exit 0; }
if [ "$mode" != tree ] && [ "$held" != 1 ]; then
    exec 8>>"$root/golden.lock"
    flock -n 8 || { echo "busy: golden lock held: $(head -c 200 "$root/golden.lock" | tr '\n' ' ')"; exit 3; }
fi
have_engine=0
command -v "$engine" >/dev/null 2>&1 && have_engine=1
if [ "$mode" != tree ]; then
    for d in "$root"/runs/*/; do
        [ -d "$d" ] || continue
        d="${d%/}"
        if [ -e "$d/active.lock" ] && ! flock -n -x "$d/active.lock" true; then
            echo "kept $d: a coordinator or supervisor holds it"; continue
        fi
        if [ "$mode" = stale ] && [ -n "$(find "$d" -maxdepth 3 \( -name done -o -name '*.done' \) -mmin -1440 -print -quit)" ]; then
            echo "kept $d: done marker under 24 h old"; continue
        fi
        kill_helpers "$d"
        if [ $have_engine = 1 ]; then
            "$engine" ps -aq --filter "label=org.openpete.fleet=${d##*/}" | xargs -r "$engine" rm -f >/dev/null
        fi
        rm -rf "$d" && echo "removed $d"
    done
    if [ $have_engine = 1 ]; then
        filt=(--filter label=org.openpete.fleet)
        [ "$mode" = stale ] && filt+=(--filter status=exited)
        for c in $("$engine" ps -aq "${filt[@]}"); do
            "$engine" rm -f "$c" >/dev/null && echo "removed container $c"
        done
    fi
fi
for k in "$@"; do
    (
        exec 6>"$root/trees/$k.lock"
        flock -n 6 || { echo "kept tree $k: a sync holds it"; exit 0; }
        rm -rf "$root/trees/$k" "$root/trees/$k.new" "$root/trees/$k.src"
        echo "removed tree $k"
    )
    [ -e "$root/trees/$k" ] || rm -f "$root/trees/$k.lock"
done
[ "$mode" = tree ] && exit 0
if [ $have_engine = 1 ]; then
    cur="$("$engine" image inspect --format '{{.Id}}' "$tag" 2>/dev/null || true)"
    for id in $(cat "$root/image.id" "$root/image.id.history" 2>/dev/null | sort -u); do
        [ "$mode" = stale ] && [ "${id#sha256:}" = "${cur#sha256:}" ] && continue
        tags="$("$engine" image inspect --format '{{join .RepoTags " "}}' "$id" 2>/dev/null)" || continue
        other=""
        for t in $tags; do case "$t" in *"$tag") ;; *) other="$other $t";; esac; done
        if [ -n "$other" ]; then echo "kept image ${id#sha256:}: also tagged$other"; continue; fi
        "$engine" rmi "$id" >/dev/null && echo "removed image ${id#sha256:}"
    done
    if [ "$mode" = all ] && "$engine" image inspect "$tag" >/dev/null 2>&1; then
        "$engine" rmi "$tag" >/dev/null && echo "untagged $tag (the image stays while another tag names it)"
    fi
fi
if [ "$mode" = all ]; then
    rm -rf "$root/trees"
    rmdir "$root/runs" 2>/dev/null || echo "kept $root/runs: a run in it is active"
    rm -f "$root/image.id" "$root/image.id.history" "$root/image.id.tmp" "$root/golden.lock"
    rmdir "$root" 2>/dev/null && echo "removed $root" || echo "kept $root: holds files the fleet did not write"
fi
exit 0
'''


def tree_key(path):
    """Return the trees/<key> name of a checkout path on this coordinator."""
    raw = f"{os.uname().nodename}\0{os.path.abspath(path)}".encode()
    return hashlib.sha256(raw).hexdigest()[:16]


def parse_tree_src(line):
    """Parse one TREE_LIST_SCRIPT line into a dict."""
    return parse_kv(line.replace("\t", "\n"))


def trees_to_remove(host, tree_path=None, now=None):
    """Return the tree keys on a host this coordinator may remove (stale, or those of tree_path)."""
    cp = host.run_script(TREE_LIST_SCRIPT, [host.fleet_root], timeout=60)
    if cp.returncode != 0:
        raise FleetError(f"{host.name}: listing trees failed: {cp.stderr.strip()[-200:]}")
    me, now = os.uname().nodename, now or time.time()
    keys = []
    for line in cp.stdout.splitlines():
        src = parse_tree_src(line)
        if src.get("coordinator") != me:
            continue
        path = src.get("path", "")
        if tree_path is not None:
            if os.path.abspath(path) == os.path.abspath(tree_path):
                keys.append(src["key"])
            continue
        age = now - float(src.get("last-run") or 0)
        if not os.path.exists(path) or age > TREE_MAX_AGE_S:
            keys.append(src["key"])
    return keys


def clean_host(host, mode="stale", tree_path=None, held=False):
    """Run clean on one host; mode stale, all or tree. Returns the script's exit code."""
    engine = host.cfg.get("engine") or ("podman" if not host.local else (local_engine() or "podman"))
    try:
        keys = trees_to_remove(host, tree_path if mode == "tree" else None)
        if mode == "all":
            keys = []  # the whole trees/ directory goes
        cp = host.run_script(CLEAN_SCRIPT, [host.fleet_root, engine, mode, int(held), IMAGE_TAG,
                                            *keys], timeout=600)
    except FleetError as e:
        print(f"[fleet] {host.name}: clean skipped: {e}")
        return EXIT_SKIP
    for line in cp.stdout.splitlines():
        print(f"[fleet] {host.name}: {line}")
    if cp.returncode not in (0, 3):
        print(f"[fleet] {host.name}: clean exited {cp.returncode}: {cp.stderr.strip()[-300:]}")
    return cp.returncode


def cmd_clean(a):
    """Remove stale runs, trees, containers and images (or everything with --all) per host."""
    _path, hosts = load_hosts()
    sel = select_hosts(hosts, a.hosts) if a.hosts else [
        Host(n, c) for n, c in hosts.items() if c.get("enabled", True)]
    if a.all and a.tree:
        raise FleetError("clean takes --all or --tree, not both", 2)
    mode = "all" if a.all else ("tree" if a.tree else "stale")
    worst = 0
    for h in sel:
        rc = clean_host(h, mode, a.tree)
        worst = max(worst, 1 if rc not in (0, 3, EXIT_SKIP) else 0)
    return worst


# ---- run: host-side scripts -------------------------------------------------

# The unit supervisor, shipped to <run dir>/ship/supervise.sh. It runs one job
# (the engine build, one unit or a canary) detached from the coordinator,
# writes <attempt>.pid (PID and start time) at launch and <attempt>.done
# (rc, status, wall time, candidate bytes) on every exit, and deletes nothing:
# the coordinator pulls results and removes the run directory.
SUPERVISOR = r'''#!/usr/bin/env bash
set -u
source "$1"
T0=$(date +%s)
mkdir -p "$UNIT_DIR"
''' + SH_STARTTIME + r'''
echo "$$ $(starttime $$)" >"$ATTEMPT_BASE.pid"
# MemAvailable at 1 Hz for the attempt's life; its minimum enters the done marker.
MEMLOG="$ATTEMPT_BASE.memavail"
while :; do awk '/^MemAvailable:/{print $2; exit}' /proc/meminfo; sleep 1; done >"$MEMLOG" 2>/dev/null &
MEMPID=$!
exec 7>>"$RUN_DIR/active.lock"
flock -s 7
helpers() {
    local p
    for p in /proc/[0-9]*; do
        [ -O "$p" ] && [ "$(cat "$p/comm" 2>/dev/null)" = dbus-daemon ] && echo "${p#/proc/} $(starttime "${p#/proc/}")"
    done | sort
}
BEFORE="$(helpers)"
# Helpers that appeared during this job are attributed to it only when no
# running container on the host lacks the fleet label; another engine user's
# helper is recorded unattributed and never signalled.
record_helpers() {
    local flag=attributed foreign
    foreign="$("$ENGINE" ps --format '{{index .Labels "org.openpete.fleet"}}' 2>/dev/null | grep -c '^$\|^<no value>$')"
    [ "${foreign:-0}" = 0 ] || flag=unattributed
    comm -13 <(echo "$BEFORE") <(helpers) | sed "s/\$/ $flag/" >>"$RUN_DIR/helpers.tsv"
}
finish() {  # rc status
    local bytes=0 wall mem
    wall=$(( $(date +%s) - T0 ))
    kill "$MEMPID" 2>/dev/null
    mem="$(sort -n "$MEMLOG" 2>/dev/null | head -n1)"
    [ -d "$UNIT_DIR/cand" ] && bytes="$(du -sb "$UNIT_DIR/cand" | cut -f1)"
    record_helpers
    printf 'rc=%s\nstatus=%s\nattempt=%s\nwall=%s\ncand-bytes=%s\nmin-memavail-kib=%s\n' \
        "$1" "$2" "$ATTEMPT" "$wall" "$bytes" "${mem:-0}" >"$ATTEMPT_BASE.done.tmp"
    mv -f "$ATTEMPT_BASE.done.tmp" "$ATTEMPT_BASE.done"
    exit 0
}
if [ "$LOCK_MODE" = take ]; then
    exec 8>>"$FLEET_ROOT/golden.lock"
    flock -n 8 || finish 75 lock-busy
    : >"$FLEET_ROOT/golden.lock"
    echo "pid $$ host $(uname -n) golden-fleet $RUN_ID $JOB since $(date -Is)" >&8
fi
export OPENPETE_FLEET_ROOT="$FLEET_ROOT" OPENPETE_GOLDEN_LOCK_HELD="$FLEET_ROOT/golden.lock"
export OPENPETE_CAPTURE_IMAGE="$IMAGE" OPENPETE_CAPTURE_LABEL="$RUN_ID"
export OPENPETE_CAPTURE_MOUNT_AS="$MOUNT_AS" OPENPETE_CONTAINER_ENGINE="$ENGINE"
# The wrapper creates and mounts the reference store on every entry (build
# included); a per-job store keeps it under fleet_root.
export OPENPETE_GOLDEN_REF_DIR="$UNIT_DIR/store"
mkdir -p "$UNIT_DIR/store"
[ -n "${PIN_LP_NUM_THREADS:-}" ] && export LP_NUM_THREADS="$PIN_LP_NUM_THREADS"
cd "$RUN_DIR/tree" || finish 1 no-tree
case "$KIND" in
build)
    export OPENPETE_CAPTURE_TARGETS="$TARGETS"
    nice -n "$NICE" scripts/golden-capture-container.sh build >"$UNIT_DIR/build.log" 2>&1 \
        || finish 1 build-failed
    for b in $BINARIES; do [ -f "$b" ] && sha256sum "$b"; done >"$UNIT_DIR/binaries.sha256"
    [ -s "$UNIT_DIR/binaries.sha256" ] || finish 1 no-binaries
    finish 0 built ;;
unit|canary)
    cand="$UNIT_DIR/cand"
    cp "$RUN_DIR/ship/fields-$GAME.txt" "$UNIT_DIR/fields.txt" || finish 1 no-fields
    export PRESENTATION_GOLDEN_OUT="$cand"
    export CORPUS_JOBS="$SLOTS" PRESENTATION_LEG_TIMEOUT="$LEG_TIMEOUT" PRESENTATION_TIER="$TIER"
    # tests/harness/check_dragon_golden.sh captures the dragons tier at stock LOD only.
    [ "$TIER" = dragons ] && export OPENPETE_CAP_LODS=original
    export OPENPETE_CAPTURE_FIELDS="$UNIT_DIR/fields.txt" OPENPETE_CAPTURE_BINARIES="$RUN_DIR/ship/binaries.sha256"
    nice -n "$NICE" tests/harness/check_presentation_golden.sh --game "$GAME" $FILTERS >"$UNIT_DIR/capture.log" 2>&1
    crc=$?
    echo "capture-rc=$crc" >>"$UNIT_DIR/capture.log"
    [ "$crc" = 64 ] && finish 64 capture-skip
    if [ -n "${POKE:-}" ]; then
        scripts/golden-capture-container.sh run -- python3 -c '
import glob, sys
from PIL import Image
fs = sorted(glob.glob(sys.argv[1] + "/*" + sys.argv[2] + "*-vk.png"))
if not fs:
    sys.exit("poke: no tile matches " + sys.argv[2])
im = Image.open(fs[0]).convert("RGB")
p = im.getpixel((0, 0))
im.putpixel((0, 0), ((p[0] + 128) % 256, p[1], p[2]))
im.save(fs[0])
print("poked", fs[0])' "$cand" "$POKE" >>"$UNIT_DIR/capture.log" 2>&1 || finish 1 poke-failed
    fi
    [ "$KIND" = canary ] && finish "$crc" canary-captured
    nice -n "$NICE" tests/harness/check_presentation_golden.sh --verdict-only "$cand" --game "$GAME" \
        >"$UNIT_DIR/verdict.log" 2>&1
    vrc=$?
    [ -f "$cand/verdict-report.txt" ] || finish "$([ "$vrc" = 64 ] && echo 64 || echo 1)" no-report
    finish "$vrc" verdict ;;
*)
    finish 1 "unknown-kind-$KIND" ;;
esac
'''

# Launch a supervisor detached, all descriptors redirected, so ssh returns at
# once. With lingering on, the supervisor is a transient unit of the user's
# systemd manager: logind with KillUserProcesses=yes (SteamOS) kills every
# process of an ssh session's scope when the session closes, setsid or not,
# and a unit under user@.service is outside that scope. systemd already makes
# the unit's main process a session leader, so it runs bash directly: setsid
# there would fork, the unit would end with its main process, and systemd
# would kill the forked child with the unit's cgroup. Without lingering the
# user manager stops with the last session, so setsid is used.
# args: supervisor envfile logfile KEY=VALUE...
LAUNCH_SCRIPT = r'''
set -eu
sup="$1"; envf="$2"; log="$3"; shift 3
mkdir -p "$(dirname "$envf")"
: >"$envf.tmp"
for kv in "$@"; do printf 'export %s=%q\n' "${kv%%=*}" "${kv#*=}" >>"$envf.tmp"; done
mv -f "$envf.tmp" "$envf"
if [ "$(loginctl show-user "$(id -un)" -p Linger --value 2>/dev/null)" = yes ] \
        && command -v systemd-run >/dev/null 2>&1; then
    unit="openpete-fleet-$(basename "$envf" .env | tr -c 'A-Za-z0-9_.-' '_')-$$"
    systemd-run --user --collect --quiet --unit="$unit" \
        bash -c 'exec bash "$0" "$1" >"$2" 2>&1 </dev/null' "$sup" "$envf" "$log"
    echo "launched $(systemctl --user show -p MainPID --value "$unit") unit $unit"
else
    setsid nohup bash "$sup" "$envf" >"$log" 2>&1 </dev/null &
    echo "launched $!"
fi
'''

# Report each attempt's state: `<base> done <k=v ...>`, `running`, `dead`
# (PID gone or reused) or `starting` (no PID file yet). args: attempt bases.
POLL_SCRIPT = SH_STARTTIME + r'''
set -u
for b in "$@"; do
    if [ -f "$b.done" ]; then echo "$b done $(tr '\n' ' ' <"$b.done")"; continue; fi
    if [ -f "$b.pid" ]; then
        read -r pid start <"$b.pid"
        if [ "$(starttime "$pid")" = "$start" ]; then echo "$b running"; else echo "$b dead"; fi
    else
        echo "$b starting"
    fi
done
'''

# Stop an attempt: every process of its supervisor's session (the supervisor
# runs under setsid, so the session id is its PID; a member started no
# earlier than the recorded start time, so a reused PID never matches), then
# its containers by the run label; then report whether the host golden lock
# is free. args: engine run-id root check-lock attempt-bases...
KILL_SCRIPT = SH_STARTTIME + r'''
set -u
engine="$1"; run="$2"; root="$3"; check="$4"; shift 4
session_members() {  # sid start
    local sid="$1" start="$2" p s f
    for p in /proc/[0-9]*; do
        s="$(cat "$p/stat" 2>/dev/null)" || continue
        read -r -a f <<<"${s##*) }"
        # f[3] is the session id, f[19] the start time (stat fields 6 and 22).
        [ "${f[3]}" = "$sid" ] && [ "${f[19]}" -ge "$start" ] && echo "${p#/proc/}"
    done
}
for b in "$@"; do
    [ -f "$b.pid" ] || continue
    read -r pid start <"$b.pid"
    members="$(session_members "$pid" "$start")"
    [ -n "$members" ] || continue
    kill -TERM $members 2>/dev/null
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        [ -n "$(session_members "$pid" "$start")" ] || break
        sleep 1
    done
    left="$(session_members "$pid" "$start")"
    [ -n "$left" ] && kill -KILL $left 2>/dev/null
    echo "stopped session $pid: $(echo $members | wc -w) process(es)"
done
for c in $("$engine" ps -aq --filter "label=org.openpete.fleet=$run" 2>/dev/null); do
    "$engine" rm -f "$c" >/dev/null 2>&1 && echo "removed container $c"
done
if [ "$check" = 1 ]; then
    if [ ! -e "$root/golden.lock" ] || flock -n "$root/golden.lock" true; then echo "lock=free"; else echo "lock=busy"; fi
else
    echo "lock=free"
fi
'''

# Delete a run directory: signal its attributed helpers, then remove it;
# names listed after the directory are kept (the coordinator's own files on
# the local host). args: run-dir keep...
DELETE_SCRIPT = SH_KILL_HELPERS + r'''
set -u
d="$1"; shift
[ -d "$d" ] || exit 0
kill_helpers "$d"
if [ $# -eq 0 ]; then rm -rf "$d"; exit 0; fi
for e in "$d"/* "$d"/.[!.]*; do
    [ -e "$e" ] || continue
    keep=0
    for k in "$@"; do [ "${e##*/}" = "$k" ] && keep=1; done
    [ $keep = 1 ] || rm -rf "$e"
done
'''

# Refresh trees/<key> from this run's snapshot by hard links, under the
# key's lock, and record who owns it. A snapshot without a build tree (a
# remote build's host before it builds) keeps the base's previous build tree,
# which seeds the next build. args: root key run-id src-lines...
REFRESH_BASE = r'''
set -eu
root="$1"; key="$2"; run="$3"; shift 3
bt=build/linux-release-container
mkdir -p "$root/trees"
exec 6>"$root/trees/$key.lock"
flock 6
rm -rf "$root/trees/$key.new"
cp -al "$root/runs/$run/tree" "$root/trees/$key.new"
if [ ! -d "$root/trees/$key.new/$bt" ] && [ -d "$root/trees/$key/$bt" ]; then
    mkdir -p "$root/trees/$key.new/build"
    mv "$root/trees/$key/$bt" "$root/trees/$key.new/$bt"
fi
# The container home and the disc-hash memo are written in place by units.
rm -rf "$root/trees/$key.new/build/linux-release-container/home" \
       "$root/trees/$key.new/build/linux-release-container/golden-disc-hash.tsv"
rm -rf "$root/trees/$key"
mv "$root/trees/$key.new" "$root/trees/$key"
printf '%s\n' "$@" >"$root/trees/$key.src"
'''

# Copy the base's build tree into the snapshot as real files (a build writes
# its objects in place, which would write through hard links into the base).
# args: root key run-id
SEED_BUILD = r'''
set -eu
root="$1"; key="$2"; run="$3"
src="$root/trees/$key/build/linux-release-container"
dst="$root/runs/$run/tree/build/linux-release-container"
if [ -d "$src" ] && [ ! -e "$dst" ]; then
    mkdir -p "$(dirname "$dst")"
    cp -a "$src" "$dst"
    echo "seeded"
else
    echo "no-seed"
fi
'''

# sha256 of each named file under a directory. args: dir files...
HASH_SCRIPT = r'''
set -u
d="$1"; shift
cd "$d" || exit 1
for f in "$@"; do
    if [ -f "$f" ]; then sha256sum "$f"; else echo "absent  $f"; fi
done
'''


# ---- run: units, ledger and timing ------------------------------------------

def load_pb(tree):
    """Import the tree's scripts/presentation-blessed.py as a module."""
    path = os.path.join(tree, "scripts", "presentation-blessed.py")
    spec = importlib.util.spec_from_file_location("presentation_blessed", path)
    mod = importlib.util.module_from_spec(spec)
    # Registered by name: digest_files pickles its worker function for a
    # process pool, and pickle resolves functions through sys.modules.
    sys.modules["presentation_blessed"] = mod
    spec.loader.exec_module(mod)
    return mod


def select_units(a):
    """Return the (game, tier) units a run's arguments name, dragons first when present."""
    games = GAMES if a.all else (a.game,)
    tiers = ("dragons", "presentation") if a.all_tiers else (a.tier,)
    units = [(g, t) for t in tiers for g in games if g in TIER_GAMES[t]]
    if not units:
        raise FleetError(f"no unit: tier {a.tier} has no game {a.game}", 2)
    return units


def store_dir():
    """Return the local reference store path the harness resolves."""
    base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    return os.path.expanduser(os.environ.get("OPENPETE_GOLDEN_REF_DIR")
                              or os.path.join(base, "openpete", "golden-ref"))


def ledger_path():
    """Return the unit-time ledger path, beside the local store."""
    return os.path.join(os.path.dirname(os.path.abspath(store_dir())), "fleet-unit-times.tsv")


def read_ledger():
    """Return {(game, tier): latest ledger row} from the unit-time ledger."""
    rows = {}
    try:
        with open(ledger_path(), encoding="utf-8") as f:
            for line in f:
                if line.startswith("#") or not line.strip():
                    continue
                row = dict(zip(LEDGER_COLS, line.rstrip("\n").split("\t")))
                rows[(row.get("game"), row.get("tier"))] = row
    except OSError:
        pass
    return rows


def append_ledger(row):
    """Append one accepted unit's time to the ledger."""
    path = ledger_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    new = not os.path.exists(path)
    with open(path, "a", encoding="utf-8") as f:
        if new:
            f.write("# " + "\t".join(LEDGER_COLS) + "\n")
        f.write("\t".join(str(row.get(k, "")) for k in LEDGER_COLS) + "\n")


def expected_s(unit, ledger):
    """Return a unit's expected wall time at speed 1.00: ledger, else scaled from the size table."""
    row = ledger.get(unit)
    if row and row.get("norm_s"):
        return float(row["norm_s"])
    return REF_UNIT_MIN * 60 * UNIT_MB[unit] / UNIT_MB[REF_UNIT]


def cand_bytes(unit, ledger):
    """Return a unit's candidate-set bytes: the last run's, else the size table."""
    row = ledger.get(unit)
    if row and row.get("cand_bytes"):
        return int(row["cand_bytes"])
    return UNIT_MB[unit] * 10**6


def lp_threads(info, slots):
    """Return LP_NUM_THREADS for a host: $LP_NUM_THREADS, else ceil(cores / slots).

    Rounding up keeps every core busy when slots does not divide the core
    count; 6 slots x 3 threads measured faster than 6 x 2 or 8 x 2 on 16 cores.
    """
    pinned = os.environ.get("LP_NUM_THREADS")
    if pinned:
        return pinned
    return str(max(1, -(-int(info.get("nproc") or os.cpu_count() or 1) // int(slots))))


def leg_timeout(speed):
    """Return PRESENTATION_LEG_TIMEOUT for a host: the default 300 s / speed x 1.5."""
    return int(round(LEG_TIMEOUT_S / speed * 1.5))


# ---- run: sync set ----------------------------------------------------------

def git_z(path, *args):
    """Return git's NUL-separated output lines for a command run in path."""
    out = subprocess.run(["git", "-C", path, *args], capture_output=True, check=True).stdout
    return [p.decode() for p in out.split(b"\0") if p]


def git_files(tree, prefix=""):
    """Return tracked plus non-ignored untracked files of tree/prefix and its checked-out submodules."""
    base = os.path.join(tree, prefix) if prefix else tree
    links = {e.split("\t", 1)[1] for e in git_z(base, "ls-files", "-s", "-z")
             if e.startswith("160000 ")}
    out = []
    for rel in git_z(base, "ls-files", "-co", "--exclude-standard", "-z"):
        full = os.path.join(prefix, rel) if prefix else rel
        if rel in links:
            if os.path.exists(os.path.join(base, rel, ".git")):
                out += git_files(tree, full)
        elif os.path.isfile(os.path.join(tree, full)):
            out.append(full)
    return out


def walk_files(tree, rel, skip_top=()):
    """Return every file under tree/rel (symlinks followed), tree-relative, minus skipped top entries."""
    top = os.path.join(tree, rel)
    out = []
    for d, dirs, files in os.walk(top, followlinks=True):
        sub = os.path.relpath(d, top)
        if sub == ".":
            dirs[:] = [x for x in dirs if x not in skip_top]
            files = [x for x in files if x not in skip_top]
        out += [os.path.normpath(os.path.join(rel, sub, f)) for f in files
                if os.path.isfile(os.path.join(d, f))]
    return out


def disc_paths(tree, pb, game):
    """Return the tree-relative cue and the bin the engine loads for it."""
    cue = pb.source_conf(f"games/{game}/oracle.conf", ("DISC",))["DISC"]
    if not cue or not os.path.isfile(os.path.join(tree, cue)):
        raise FleetError(f"{game}: no disc cue {cue or '(DISC unset)'} in {tree}", EXIT_SKIP)
    bin_rel = pb.resolve_disc_bin(cue)
    if not bin_rel:
        raise FleetError(f"{game}: no .bin resolves for {cue}", EXIT_SKIP)
    return os.path.normpath(cue), bin_rel


def source_set(tree, pb, games):
    """Return the positive sync set without the build tree: git files plus the listed ignored inputs."""
    files = set(git_files(tree))
    for g in GAMES:
        files.update(walk_files(tree, f"games/{g}/generated"))
        for t in ("globals.toml", "symbols.toml"):
            rel = f"games/{g}/manifest/{t}"
            if os.path.isfile(os.path.join(tree, rel)):
                files.add(rel)
    for g in games:
        files.update(disc_paths(tree, pb, g))
    files.update(walk_files(tree, "third_party/psxrecomp/build"))
    return sorted(files)


def build_set(root):
    """Return the build tree's files under root minus the container home and the disc-hash memo."""
    return sorted(walk_files(root, BUILD_TREE, skip_top=("home", "golden-disc-hash.tsv")))


def set_bytes(root, files):
    """Return the summed size of a file list under root."""
    return sum(os.path.getsize(os.path.join(root, f)) for f in files)


def write_list(path, files):
    """Write a NUL-separated --files-from list."""
    with open(path, "wb") as f:
        f.write(b"".join(x.encode() + b"\0" for x in files))


def binary_paths(root):
    """Return the tree-relative game binaries present under root's build tree."""
    rels = [f"{BUILD_TREE}/games/{g}/openpete-{g.replace('-', '')}" for g in GAMES]
    return [r for r in rels if os.path.isfile(os.path.join(root, r))]


def sha256_path(path):
    """Return a file's sha256, or '' when absent."""
    if not os.path.isfile(path):
        return ""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---- run: transfer ----------------------------------------------------------

def rsync_base(host):
    """Return rsync's transport arguments for a host."""
    return [] if host.local else ["-e", "ssh " + " ".join(SSH_OPTS)]


def remote_path(host, path):
    """Return an rsync operand naming path on the host."""
    return path if host.local else f"{host.cfg['ssh']}:{path}"


def rsync_push(host, src, listfile, dest, link_dest=None):
    """Copy a --files-from list from local src to dest on the host (symlinks dereferenced)."""
    argv = ["rsync", "-a", "-L", "--from0", f"--files-from={listfile}", "--stats",
            *rsync_base(host)]
    if link_dest:
        argv.append(f"--link-dest={link_dest}")
    argv += [src.rstrip("/") + "/", remote_path(host, dest.rstrip("/")) + "/"]
    t0 = time.monotonic()
    cp = subprocess.run(argv, capture_output=True, text=True)
    if cp.returncode != 0:
        raise FleetError(f"{host.name}: rsync to {dest} exited {cp.returncode}: "
                         f"{cp.stderr.strip()[-400:]}")
    sent = re.search(r"Total bytes sent: ([\d,]+)", cp.stdout)
    return time.monotonic() - t0, int(sent.group(1).replace(",", "")) if sent else 0


def rsync_recheck(host, src, listfile, dest):
    """Return the content changes between local src and dest over a list (empty when equal)."""
    argv = ["rsync", "-n", "-i", "-c", "-L", "--from0", f"--files-from={listfile}",
            *rsync_base(host), src.rstrip("/") + "/", remote_path(host, dest.rstrip("/")) + "/"]
    cp = subprocess.run(argv, capture_output=True, text=True)
    changes = [ln for ln in cp.stdout.splitlines()
               if ln[:1] in "<>ch*" and ln[1:2] in "fLS" or ln.startswith("*deleting")]
    if cp.returncode != 0:
        changes.append(f"rsync exited {cp.returncode}: {cp.stderr.strip()[-300:]}")
    return changes


def rsync_pull(host, src_dir, dst_dir, names=None, includes=None):
    """Copy named files (or include patterns, or everything) from src_dir on the host to dst_dir."""
    os.makedirs(dst_dir, exist_ok=True)
    argv = ["rsync", "-a", *rsync_base(host)]
    lst = None
    if names is not None:
        lst = os.path.join(dst_dir, ".pull-list")
        write_list(lst, names)
        argv += ["--from0", f"--files-from={lst}"]
    for pat in includes or ():
        argv.append(f"--include={pat}")
    if includes:
        argv.append("--exclude=*")
    argv += [remote_path(host, src_dir.rstrip("/")) + "/", dst_dir.rstrip("/") + "/"]
    cp = subprocess.run(argv, capture_output=True, text=True)
    if lst:
        os.unlink(lst)
    return cp.returncode, cp.stderr.strip()[-300:]


# ---- run: the checkout-reading span -------------------------------------------

class CheckoutSpan:
    """Marker process plus the checkout's container build lock while the fleet reads the checkout."""

    def __init__(self, tree):
        """Bind the checkout; nothing is held yet."""
        self.tree = tree
        self.marker = None
        self.lock_fd = None

    def start_marker(self):
        """Start the `golden-fleet-sync` marker process a build lock tests for."""
        if self.marker is None:
            self.marker = subprocess.Popen(
                [sys.executable, "-c", "import time\nwhile True: time.sleep(3600)",
                 "golden-fleet-sync", self.tree], cwd=self.tree,
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def take_build_lock(self):
        """Take build/linux-release-container.build-lock, waiting while a build holds it."""
        path = os.path.join(self.tree, "build", "linux-release-container.build-lock")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.lock_fd = os.open(path, os.O_WRONLY | os.O_CREAT, 0o644)
        try:
            fcntl.flock(self.lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print(f"[fleet] {path} held by a build; waiting")
            fcntl.flock(self.lock_fd, fcntl.LOCK_EX)

    def release(self):
        """Release the build lock and stop the marker."""
        if self.lock_fd is not None:
            os.close(self.lock_fd)
            self.lock_fd = None
        if self.marker is not None:
            self.marker.terminate()
            self.marker.wait()
            self.marker = None


# ---- run: state ---------------------------------------------------------------

class Job:
    """One placement: the engine build, one host's canary, or a (game, tier) unit."""

    def __init__(self, kind, game=None, tier="presentation", pinned=None, slots=None, label=None):
        """Bind the job's kind, unit, pinned host, and a calibration leg's slots and name."""
        self.kind, self.game, self.tier, self.pinned = kind, game, tier, pinned
        self.slots, self.label = slots, label
        self.attempts = 0
        self.result = None

    @property
    def unit(self):
        """The (game, tier) key of a unit or canary."""
        return (self.game, self.tier)

    @property
    def name(self):
        """The job's directory name under the run directory."""
        if self.label:
            return self.label
        if self.kind == "build":
            return "build"
        if self.kind == "canary":
            return f"canary-{self.pinned}"
        return f"{self.game}-{self.tier}"


class Attempt:
    """One placement of a job on a host, identified by its attempt number."""

    def __init__(self, job, hs, deadline_s):
        """Start the attempt's clocks."""
        job.attempts += 1
        self.job, self.hs, self.n = job, hs, job.attempts
        self.t0 = time.monotonic()
        self.deadline = self.t0 + deadline_s
        self.lost_since = None

    @property
    def unit_dir(self):
        """The job's directory on the host."""
        return f"{self.hs.run_dir}/u-{self.job.name}"

    @property
    def base(self):
        """The attempt's pid/done file prefix on the host."""
        return f"{self.unit_dir}/attempt-{self.n}"


class HostState:
    """A usable host within one run."""

    def __init__(self, host, info, slots, run_id):
        """Bind the host, its probe, its slots and its run directory."""
        self.host, self.info, self.slots = host, info, slots
        self.name = host.name
        self.root = info["root"]
        self.speed = float(host.cfg.get("speed", 1.0))
        self.run_dir = f"{self.root}/runs/{run_id}"
        self.nice = int(host.cfg.get("nice", 19 if host.local else 0))
        self.engine = info.get("engine") or "podman"
        self.current = None
        self.dropped = None
        self.touched = False


# ---- run: the coordinator -----------------------------------------------------

class Interrupted(FleetError):
    """A signal ended the run."""


class Coordinator:
    """One `run`: hosts, build, sync, placement, verdicts, pulls and cleanup."""

    def __init__(self, a, run_id, local_run_dir):
        """Bind the arguments and the run's identity."""
        self.a, self.run_id = a, run_id
        self.tree = script_repo()
        self.pb = load_pb(self.tree)
        self.units = select_units(a)
        self.ledger = read_ledger()
        self.local_run_dir = local_run_dir
        self.ship_dir = os.path.join(local_run_dir, "ship")
        self.hosts = []
        self.golden_fd = None
        self.span = CheckoutSpan(self.tree)
        self.jobs = []
        self.rc_notes = []
        self.poke = self.parse_poke(os.environ.get("OPENPETE_FLEET_POKE", ""))

    @staticmethod
    def parse_poke(spec):
        """Parse OPENPETE_FLEET_POKE=<host>:<job>:<tile substring> (a negative-control knob)."""
        if not spec:
            return None
        parts = spec.split(":", 2)
        if len(parts) != 3:
            raise FleetError(f"OPENPETE_FLEET_POKE={spec}: expected <host>:<job>:<tile>", 2)
        return tuple(parts)

    # -- hosts --

    def pick_hosts(self, hosts):
        """Return the Host objects this run considers: --hosts, else defaults, minus --no-local."""
        if self.a.hosts:
            sel = select_hosts(hosts, self.a.hosts)
        else:
            sel = [Host(n, c) for n, c in hosts.items()
                   if c.get("enabled", True) and c.get("default", True)]
        if self.a.no_local:
            sel = [h for h in sel if not h.local]
        if not sel:
            raise FleetError("no host selected (every host is disabled, not default, or local "
                             "under --no-local)", EXIT_SKIP)
        return sel

    def need_bytes(self, src_bytes):
        """Return the free space a host needs: sync set, build tree and the largest candidate set."""
        build = os.path.join(self.tree, BUILD_TREE)
        build_bytes = set_bytes(self.tree, build_set(self.tree)) if os.path.isdir(build) \
            else BUILD_TREE_BYTES
        return src_bytes + build_bytes + max(cand_bytes(u, self.ledger) for u in self.units)

    def prepare_hosts(self, src_bytes):
        """Preflight, take the local golden lock, clean each host and ship the image."""
        _path, hosts = load_hosts()
        sel = self.pick_hosts(hosts)
        check_recipe(self.tree)
        results = preflight(sel, self.need_bytes(src_bytes))
        usable = print_preflight(results)
        infos = {h.name: i for h, i, _r, _s in results}
        for h, slots in usable:
            hs = HostState(h, infos[h.name], slots, self.run_id)
            if h.local:
                self.golden_fd = try_golden_lock(h.fleet_root)
                if self.golden_fd is None:
                    print(f"[fleet] {h.name}: dropped: golden lock taken since preflight")
                    continue
            rc = clean_host(h, "stale", held=h.local)
            if rc != 0:
                print(f"[fleet] {h.name}: dropped: start-of-run clean exited {rc}")
                continue
            ship_image(h, infos[h.name])
            self.hosts.append(hs)
        if not self.hosts:
            raise FleetError("no usable host", EXIT_SKIP)
        builders = [hs for hs in self.hosts if hs.host.cfg.get("build", True)]
        if not builders:
            raise FleetError("no usable host may build (build = true)", EXIT_SKIP)
        self.build_hs = max(builders, key=lambda hs: (hs.speed, hs.host.local))
        print(f"[fleet] hosts: {', '.join(f'{hs.name} (slots {hs.slots}, speed {hs.speed:.2f})' for hs in self.hosts)}; "
              f"build host {self.build_hs.name}")

    # -- small remote helpers --

    def script(self, hs, script, args, timeout=300, check=True):
        """Run a script on a host; on failure raise naming the host."""
        return hs.host.run_script(script, args, timeout=timeout, check=check)

    def mkdirs(self, hs):
        """Create the run directory on a host and mark it touched."""
        self.script(hs, "set -eu; mkdir -p \"$1/ship\" \"$1/tree\"", [hs.run_dir])
        hs.touched = True

    def push_ship(self, hs):
        """Copy the ship directory (supervisor, fields, binary hashes) into the host's run directory."""
        if hs.host.local:
            return
        files = sorted(os.listdir(self.ship_dir))
        lst = os.path.join(self.local_run_dir, "ship.lst")
        write_list(lst, files)
        rsync_push(hs.host, self.ship_dir, lst, f"{hs.run_dir}/ship")

    def refresh_base(self, hs):
        """Refresh the host's trees/<key> from this run's snapshot."""
        src = [f"coordinator={os.uname().nodename}", f"path={self.tree}",
               f"last-run={int(time.time())}"]
        self.script(hs, REFRESH_BASE, [hs.root, tree_key(self.tree), self.run_id, *src],
                    timeout=1800)

    def base_exists(self, hs):
        """True when the host holds trees/<key>."""
        cp = self.script(hs, 'test -d "$1"', [f"{hs.root}/trees/{tree_key(self.tree)}"],
                         check=False)
        return cp.returncode == 0

    # -- sync --

    def sync_segment(self, hs, src_root, files, label, link_dest=True):
        """Copy one segment's list to the host's snapshot with --link-dest from its base."""
        lst = os.path.join(self.local_run_dir, f"{label}.lst")
        write_list(lst, files)
        base = f"{hs.root}/trees/{tree_key(self.tree)}" if link_dest and self.base_exists(hs) else None
        secs, sent = rsync_push(hs.host, src_root, lst, f"{hs.run_dir}/tree", base)
        print(f"[fleet] {hs.name}: synced {label}: {len(files)} files, {sent / 2**20:.0f} MiB "
              f"sent in {secs:.0f} s" + ("" if base else " (no base)"))
        return lst

    def recheck(self, hs, src_root, lst, label):
        """Refuse the run when the host's snapshot differs from src_root over a segment's list."""
        changes = rsync_recheck(hs.host, src_root, lst, f"{hs.run_dir}/tree")
        if changes:
            shown = "\n  ".join(changes[:20])
            raise FleetError(f"torn sync: {hs.name} snapshot differs from {src_root} over the "
                             f"{label} list ({len(changes)} change(s)):\n  {shown}")
        print(f"[fleet] {hs.name}: recheck {label}: 0 changes")

    def after_sync_hook(self):
        """Run OPENPETE_FLEET_AFTER_SYNC (a negative-control knob) in the checkout."""
        cmd = os.environ.get("OPENPETE_FLEET_AFTER_SYNC")
        if cmd:
            print(f"[fleet] OPENPETE_FLEET_AFTER_SYNC: {cmd}")
            subprocess.run(["bash", "-c", cmd], cwd=self.tree, check=False)

    def write_ship(self):
        """Write the supervisor and each run game's capture fields into the ship directory."""
        os.makedirs(self.ship_dir, exist_ok=True)
        with open(os.path.join(self.ship_dir, "supervise.sh"), "w") as f:
            f.write(SUPERVISOR)
        for g in self.run_games():
            out = os.path.join(self.ship_dir, f"fields-{g}.txt")
            subprocess.run([sys.executable, "scripts/presentation-blessed.py", "capture-fields",
                            g, "--out", out], cwd=self.tree, check=True)

    def run_games(self):
        """Return the games any job of this run captures."""
        return sorted({g for g, _t in self.units} | ({self.canary_game()} if self.canary_on() else set()))

    def write_binaries(self, root):
        """Write ship/binaries.sha256 for the binaries under root."""
        lines = [f"{sha256_path(os.path.join(root, r))}  {r}\n" for r in binary_paths(root)]
        if not lines:
            raise FleetError(f"no game binary under {root}/{BUILD_TREE}")
        with open(os.path.join(self.ship_dir, "binaries.sha256"), "w") as f:
            f.writelines(lines)
        print("[fleet] binaries: " + ", ".join(ln.split()[1].rsplit('/', 1)[1] + " "
                                                + ln[:12] for ln in lines))

    def local_build(self):
        """Build the engine in the checkout through the wrapper, logged in the run directory."""
        log = os.path.join(self.local_run_dir, "build.log")
        env = dict(os.environ, OPENPETE_CAPTURE_IMAGE=IMAGE_TAG,
                   OPENPETE_CAPTURE_TARGETS=" ".join(GAME_TARGETS))
        print(f"[fleet] {self.build_hs.name}: in-image build in {self.tree} (log {log})")
        t0 = time.monotonic()
        with open(log, "w") as f:
            rc = subprocess.run(["nice", "-n", str(self.build_hs.nice),
                                 "scripts/golden-capture-container.sh", "build"], cwd=self.tree,
                                env=env, stdout=f, stderr=subprocess.STDOUT).returncode
        if rc != 0:
            raise FleetError(f"in-image build failed (exit {rc}); log {log}")
        print(f"[fleet] build done in {time.monotonic() - t0:.0f} s")

    def build_and_sync(self):
        """Build once on the build host and give every host an identical snapshot (steps 3-4)."""
        for hs in self.hosts:
            self.mkdirs(hs)
        local_build = self.build_hs.host.local
        self.span.start_marker()
        try:
            if local_build:
                self.local_build()
            self.span.take_build_lock()
            segs = [("source", self.tree, source_set(self.tree, self.pb, self.run_games()))]
            if local_build:
                segs.append(("build", self.tree, build_set(self.tree)))
            self.sync_hosts(segs, self.hosts)
            # Measured while the checkout equals every snapshot (the recheck just passed).
            self.write_ship()
            if local_build:
                self.write_binaries(self.tree)
        finally:
            self.span.release()
        self.refresh_bases(self.hosts)
        if not local_build:
            self.remote_build()
        for hs in self.hosts:
            self.push_ship(hs)
        self.check_discs()

    def sync_hosts(self, segs, hosts):
        """Sync every segment to every host, run the after-sync hook, then recheck every copy."""
        lists = {}
        for hs in hosts:
            for label, root, files in segs:
                lists[(hs.name, label)] = self.sync_segment(hs, root, files, label)
        self.after_sync_hook()
        for hs in hosts:
            for label, root, _files in segs:
                self.recheck(hs, root, lists[(hs.name, label)], label)

    def refresh_bases(self, hosts):
        """Refresh each host's link-dest base from its complete snapshot."""
        for hs in hosts:
            self.refresh_base(hs)

    def remote_build(self):
        """Build in the build host's snapshot, then relay its build tree to the other hosts."""
        bh = self.build_hs
        cp = self.script(bh, SEED_BUILD, [bh.root, tree_key(self.tree), self.run_id], timeout=1800)
        print(f"[fleet] {bh.name}: build tree {cp.stdout.strip()} from its base")
        self.push_ship(bh)
        job = Job("build", pinned=bh.name)
        self.run_jobs([job], deadline=lambda j, hs: BUILD_DEADLINE_S / hs.speed)
        if not job.result or job.result["rc"] != 0:
            raise FleetError(f"{bh.name}: in-image build failed: "
                             f"{(job.result or {}).get('status', 'no result')}; log "
                             f"{bh.run_dir}/u-build/build.log")
        rc, err = rsync_pull(bh.host, f"{bh.run_dir}/u-build", self.ship_dir,
                             names=["binaries.sha256"])
        if rc != 0:
            raise FleetError(f"{bh.name}: pulling binaries.sha256 failed: {err}")
        print(f"[fleet] {bh.name}: built in {job.result['wall']} s")
        self.refresh_base(bh)
        others = [hs for hs in self.hosts if hs is not bh]
        if others:
            self.relay_build(bh, others)

    def relay_build(self, bh, others):
        """Pull the build tree from the build host, check its binaries, sync it to the others."""
        relay = os.path.join(self.local_run_dir, "relay")
        rc, err = rsync_pull(bh.host, f"{bh.run_dir}/tree/{BUILD_TREE}",
                             os.path.join(relay, BUILD_TREE))
        if rc != 0:
            raise FleetError(f"{bh.name}: pulling the build tree failed: {err}")
        check = subprocess.run(["sha256sum", "-c", "--strict", "--quiet",
                                os.path.join(self.ship_dir, "binaries.sha256")],
                               cwd=relay, capture_output=True, text=True)
        if check.returncode != 0:
            raise FleetError(f"relayed build tree differs from the build host's binary hashes: "
                             f"{check.stdout.strip()[-300:]}")
        self.sync_hosts([("build", relay, build_set(relay))], others)
        self.refresh_bases(others)

    def check_discs(self):
        """Refuse the run unless every host's synced disc bytes hash to the blessed header's."""
        for g in self.run_games():
            cue, bin_rel = disc_paths(self.tree, self.pb, g)
            want = self.blessed_disc(g)
            for hs in self.hosts:
                cp = self.script(hs, HASH_SCRIPT, [f"{hs.run_dir}/tree", cue, bin_rel])
                got = {ln.split(None, 1)[1]: ln.split(None, 1)[0]
                       for ln in cp.stdout.splitlines() if ln.strip()}
                for key, rel in (("disc-cue", cue), ("disc-bin", bin_rel)):
                    if want[key] and got.get(rel) != want[key]:
                        raise FleetError(f"{hs.name}: synced {rel} hashes to {got.get(rel)}, "
                                         f"blessed header {key} is {want[key]}")
            print(f"[fleet] {g}: disc cue and bin hash to the blessed header on every host")

    def blessed_disc(self, game):
        """Return the disc-cue and disc-bin sha256 the game's unit headers record (consistent across tiers)."""
        want = {"disc-cue": "", "disc-bin": ""}
        for tier in {t for g, t in self.units if g == game} | {"presentation"}:
            if game not in TIER_GAMES.get(tier, ()):
                continue
            header = self.pb.load_manifest(game, tier).header
            for key in want:
                sha = (header.get(key) or "").split(" ")[0]
                if want[key] and sha and sha != want[key]:
                    raise FleetError(f"{game}: blessed headers disagree on {key}")
                want[key] = want[key] or sha
        return want

    # -- canary --

    def canary_on(self):
        """True when the run spans two or more capture hosts."""
        return len(self.hosts) >= 2

    def canary_game(self):
        """Return the canary's game: the first unit's game."""
        return self.units[0][0]

    # -- placement --

    def make_jobs(self):
        """Return the run's jobs: one canary per host when on, then units longest first."""
        jobs = []
        if self.canary_on():
            jobs += [Job("canary", self.canary_game(), "presentation", pinned=hs.name)
                     for hs in self.hosts]
        units = sorted(self.units, key=lambda u: -expected_s(u, self.ledger))
        jobs += [Job("unit", g, t) for g, t in units]
        return jobs

    def deadline(self, job, hs):
        """Return a job's deadline in seconds on a host (first-run rule from the size table)."""
        if job.kind == "canary":
            return CANARY_DEADLINE_S / hs.speed * 2
        return expected_s(job.unit, self.ledger) / hs.speed * 2

    def leg_timeout_s(self, _job, hs):
        """Return a job's PRESENTATION_LEG_TIMEOUT on a host."""
        return leg_timeout(hs.speed)

    def job_slots(self, job, hs):
        """Return a job's CORPUS_JOBS: its own slots (a calibration leg), else the host's."""
        return job.slots or hs.slots

    def job_env(self, job, hs, att):
        """Return the KEY=VALUE list a supervisor sources."""
        game, tier = job.game or "", job.tier
        slots = self.job_slots(job, hs)
        canary = CANARY.get(game, ("", "")) if job.kind == "canary" else None
        filters = f"--filter={canary[0]} --freecam-filter={canary[1]}" if canary else ""
        poke = ""
        if self.poke and self.poke[0] == hs.name and self.poke[1] in (job.name, job.kind):
            poke = self.poke[2]
        env = {"RUN_ID": self.run_id, "JOB": job.name, "KIND": job.kind, "GAME": game,
               "TIER": tier, "RUN_DIR": hs.run_dir, "UNIT_DIR": att.unit_dir,
               "ATTEMPT": att.n, "ATTEMPT_BASE": att.base, "FLEET_ROOT": hs.root,
               "LOCK_MODE": "held" if hs.host.local else "take", "IMAGE": IMAGE_TAG,
               "MOUNT_AS": self.tree, "ENGINE": hs.engine, "NICE": hs.nice,
               "SLOTS": slots, "LEG_TIMEOUT": self.leg_timeout_s(job, hs), "FILTERS": filters,
               "POKE": poke, "BINARIES": " ".join(
                   f"{BUILD_TREE}/games/{g}/openpete-{g.replace('-', '')}" for g in GAMES),
               "TARGETS": " ".join(GAME_TARGETS),
               "PIN_LP_NUM_THREADS": lp_threads(hs.info, slots)}
        return [f"{k}={v}" for k, v in env.items()]

    def launch(self, job, hs, deadline_s):
        """Start one attempt of a job on a host through its detached supervisor."""
        att = Attempt(job, hs, deadline_s)
        args = [f"{hs.run_dir}/ship/supervise.sh", f"{att.base}.env", f"{att.base}.log",
                *self.job_env(job, hs, att)]
        cp = self.script(hs, LAUNCH_SCRIPT, args, timeout=60, check=False)
        if cp.returncode != 0:
            job.attempts -= 1
            self.drop(hs, f"launch failed: {cp.stderr.strip()[-200:]}")
            return None
        hs.current = att
        slots = self.job_slots(job, hs)
        print(f"[fleet] {hs.name}: {job.name} attempt {att.n} started "
              f"(slots {slots}, LP_NUM_THREADS {lp_threads(hs.info, slots)}, leg timeout "
              f"{self.leg_timeout_s(job, hs)} s, deadline {deadline_s / 60:.0f} min)")
        return att

    def drop(self, hs, why):
        """Drop a host for the rest of the run."""
        hs.dropped = why
        print(f"[fleet] {hs.name}: dropped for the rest of the run: {why}")

    def next_job(self, pending, hs):
        """Return the first pending job this host may take, or None."""
        for job in pending:
            if job.pinned in (None, hs.name):
                return job
        return None

    def run_jobs(self, jobs, deadline=None, on_done=None):
        """Place jobs on free hosts until every job has a result; on_done may return follow-up jobs."""
        pending = list(jobs)
        deadline = deadline or self.deadline
        while True:
            self.place(pending, deadline)
            running = [hs for hs in self.hosts if hs.current]
            if not running:
                break
            time.sleep(POLL_S)
            for hs in running:
                self.poll(hs, pending, on_done)
        for job in pending:
            job.result = {"rc": EXIT_SKIP, "status": "no usable host left", "host": "-"}
            print(f"[fleet] {job.name}: SKIP: no usable host left")

    def place(self, pending, deadline):
        """Give each free, undropped host the next job it may take."""
        for hs in sorted(self.hosts, key=lambda h: -h.speed):
            if hs.current or hs.dropped:
                continue
            job = self.next_job(pending, hs)
            if job and self.launch(job, hs, deadline(job, hs)):
                pending.remove(job)
        for job in [j for j in pending if j.pinned]:
            hs = next((h for h in self.hosts if h.name == job.pinned), None)
            if hs is None or hs.dropped:
                pending.remove(job)
                job.result = {"rc": EXIT_SKIP, "status": f"host {job.pinned} dropped",
                              "host": job.pinned}

    def poll(self, hs, pending, on_done):
        """Poll a host's attempt and act on done, dead, overdue or unreachable."""
        att = hs.current
        try:
            cp = hs.host.run_script(POLL_SCRIPT, [att.base], timeout=60)
            state = cp.stdout.split(None, 2)[1:] if cp.returncode == 0 else []
        except FleetError:
            state = []
        now = time.monotonic()
        if not state:
            att.lost_since = att.lost_since or now
            if now - att.lost_since > GRACE_S:
                self.drop(hs, f"unreachable for {GRACE_S // 60} min during {att.job.name}; "
                              f"its runs/ directory stays for clean")
                self.requeue(att, pending, killed=False)
            return
        att.lost_since = None
        if state[0] == "done":
            self.finish_attempt(att, parse_kv(state[1].replace(" ", "\n")), pending, on_done)
        elif state[0] == "dead" or (state[0] == "starting" and now - att.t0 > START_GRACE_S):
            print(f"[fleet] {hs.name}: {att.job.name} attempt {att.n}: supervisor {state[0]} "
                  f"without a done marker")
            self.stop_and_requeue(att, pending)
        elif now > att.deadline:
            print(f"[fleet] {hs.name}: {att.job.name} attempt {att.n} past its deadline "
                  f"({(att.deadline - att.t0) / 60:.0f} min)")
            self.stop_and_requeue(att, pending)

    def stop_and_requeue(self, att, pending):
        """Kill an attempt's supervisor and containers, confirm the lock is free, re-queue."""
        hs = att.hs
        try:
            cp = hs.host.run_script(KILL_SCRIPT, [hs.engine, self.run_id, hs.root,
                                                  int(not hs.host.local), att.base], timeout=120)
            out = cp.stdout
        except FleetError as e:
            out = str(e)
        for line in out.splitlines():
            if not line.startswith("lock="):
                print(f"[fleet] {hs.name}: {line}")
        if "lock=free" not in out:
            self.drop(hs, "could not confirm the golden lock released after a kill")
        self.requeue(att, pending, killed=True)

    def requeue(self, att, pending, killed):
        """Re-queue an attempt's job while attempts remain, else record its failure."""
        att.hs.current = None
        job = att.job
        if job.attempts < MAX_ATTEMPTS and job.kind != "build":
            pending.insert(0, job)
            print(f"[fleet] {job.name}: re-queued (attempt {job.attempts} of {MAX_ATTEMPTS} lost)")
        else:
            job.result = {"rc": 1, "status": "attempts exhausted" if killed else "host lost",
                          "host": att.hs.name, "attempt": att.n}

    def finish_attempt(self, att, done, pending, on_done):
        """Accept the current attempt's done marker (or drop a busy host) and record the result."""
        hs, job = att.hs, att.job
        hs.current = None
        if str(done.get("attempt")) != str(att.n):
            print(f"[fleet] {hs.name}: {job.name}: done marker of attempt {done.get('attempt')} "
                  f"ignored (current {att.n})")
            self.stop_and_requeue(att, pending)
            return
        rc = int(done.get("rc", 1))
        if rc == 75:
            job.attempts -= 1
            pending.insert(0, job)
            self.drop(hs, "golden lock busy")
            return
        job.result = {"rc": rc, "status": done.get("status", ""), "host": hs.name,
                      "attempt": att.n, "wall": int(done.get("wall", 0)),
                      "cand_bytes": int(done.get("cand-bytes", 0)), "speed": hs.speed,
                      "unit_dir": att.unit_dir,
                      "min_memavail_kib": int(done.get("min-memavail-kib") or 0)}
        print(f"[fleet] {hs.name}: {job.name} attempt {att.n} finished: rc {rc} "
              f"({job.result['status']}) in {job.result['wall']} s")
        if on_done:
            pending.extend(on_done(job, hs) or ())

    # -- verdict and pull (step 9) --

    def pull_target(self, game, tier):
        """Return the local candidate directory a unit's results are pulled into."""
        out = os.environ.get("PRESENTATION_GOLDEN_OUT")
        if out:
            return out if len(self.units) == 1 else os.path.join(out, f"{game}-{tier}")
        prefix = "presentation-candidate" if tier == "presentation" else "dragon-candidate"
        name = prefix if game == "spyro-1" else f"{prefix}-{game}"
        return os.path.join(self.tree, "build", name)

    def unit_done(self, job, hs):
        """Pull, check and render a finished unit; canaries pull their tiles only."""
        if job.kind == "canary":
            dst = os.path.join(self.local_run_dir, "canary", hs.name)
            shutil.rmtree(dst, ignore_errors=True)
            rc, err = rsync_pull(hs.host, f"{job.result['unit_dir']}/cand", dst,
                                 includes=["*-vk.png"])
            if rc != 0:
                job.result.update(rc=1, status=f"canary pull failed: {err}")
            return
        if job.result["rc"] in (0, 1):
            errs = self.pull_unit(job, hs)
            if errs:
                job.result.update(rc=1, status="; ".join(errs))
                for e in errs:
                    print(f"[fleet] {job.name}: REFUSED: {e}")
            elif job.result["rc"] in (0, 1):
                self.record_time(job)

    def pull_unit(self, job, hs):
        """Pull a unit's report and divergences into a cleared local directory; return refusals."""
        local = self.pull_target(job.game, job.tier)
        shutil.rmtree(local, ignore_errors=True)
        os.makedirs(local)
        rdir = f"{job.result['unit_dir']}/cand"
        first = ["verdict-report.txt", "capture-record.txt", "capture-device.txt",
                 "verdict-ident.log", "verdict-diff.log"]
        if job.tier == "presentation":
            first.append(CREDITS_NAME)
        if self.a.pull == "all":
            rsync_pull(hs.host, rdir, local)
        else:
            rsync_pull(hs.host, rdir, local, names=first)
        report = parse_report(os.path.join(local, "verdict-report.txt"))
        tiles, pairs = report["divergent"], report["identity"]
        if self.a.pull != "all":
            rsync_pull(hs.host, rdir, local, names=tiles)
            if pairs:
                rsync_pull(hs.host, rdir, os.path.join(local, "identity"), names=pairs)
        want = first + tiles + [os.path.join("identity" if self.a.pull != "all" else "", p)
                                for p in pairs]
        errs = [f"pulled file absent: {f}" for f in want
                if not os.path.exists(os.path.join(local, f))]
        errs += self.check_report(job, report)
        print(f"[fleet] {job.name}: pulled into {local}: report, record, device, credits, "
              f"{len(tiles)} divergent tile(s), {len(pairs)} identity tile(s)")
        if tiles and not errs:
            self.heat_diffs(job, local)
        return errs

    def check_report(self, job, report):
        """Compare the report's manifest hashes and census with the local files."""
        golden = os.path.join(self.tree, "tests", "golden", job.game, job.tier)
        errs = []
        if not report["fields"]:
            return ["verdict-report.txt absent or empty"]
        for key, name in (("blessed-sha256", "blessed.tsv"),
                          ("ident-baseline-sha256", "ident-baseline.tsv")):
            got = first_hex(report["fields"].get(key, ""))
            want = sha256_path(os.path.join(golden, name))
            if got != want:
                errs.append(f"{key} {got[:12] or 'absent'} differs from the local {name} "
                            f"{want[:12] or 'absent'}")
        rows = len(self.pb.load_manifest(job.game, job.tier).rows)
        compared = report["fields"].get("compared", "")
        if compared != str(rows):
            errs.append(f"census compared={compared or 'absent'} differs from the local "
                        f"blessed row count {rows}")
        if report["fields"].get("exit") != str(job.result["rc"]):
            errs.append(f"report exit {report['fields'].get('exit')} differs from the "
                        f"supervisor's rc {job.result['rc']}")
        return errs

    def heat_diffs(self, job, local):
        """Render heat diffs of the pulled divergent tiles against the local store."""
        blessed = os.path.join("tests", "golden", job.game, job.tier, "blessed.tsv")
        cp = subprocess.run([sys.executable, "tests/harness/presentation-corpus-diff.py", local,
                             "--game", job.game, "--manifest", blessed, "--diff-out",
                             os.path.join(local, "diff"), "--expect-subset"],
                            cwd=self.tree, capture_output=True, text=True)
        for line in cp.stdout.splitlines():
            if not line.startswith("ok "):
                print(f"[fleet] {job.name}: {line}")

    def record_time(self, job):
        """Append an accepted unit's time to the ledger."""
        r = job.result
        append_ledger({"date": datetime.datetime.now().isoformat(timespec="seconds"),
                       "run": self.run_id, "game": job.game, "tier": job.tier,
                       "host": r["host"], "speed": f"{r['speed']:.2f}", "wall_s": r["wall"],
                       "norm_s": int(r["wall"] * r["speed"]), "cand_bytes": r["cand_bytes"]})

    def compare_canaries(self, jobs):
        """Refuse the run when a canary tile's digest differs between hosts (step 8)."""
        done = [j for j in jobs if j.kind == "canary" and j.result and j.result["rc"] in (0, 1)]
        if len(done) < 2:
            if self.canary_on():
                print(f"[fleet] canary: {len(done)} host(s) captured it; no cross-host comparison")
            return 0
        digests = {j.pinned: self.canary_digests(j.pinned) for j in done}
        names = sorted(set().union(*digests.values()))
        bad = [n for n in names if len({digests[h].get(n, "absent") for h in digests}) > 1]
        hosts = ", ".join(sorted(digests))
        if not names:
            print(f"[fleet] canary: VACUOUS: no canary tile on {hosts}")
            return 1
        if bad:
            for n in bad:
                print(f"[fleet] canary: {n}: " + ", ".join(
                    f"{h}={digests[h].get(n, 'absent')[:12]}" for h in sorted(digests)))
            print(f"[fleet] canary: REFUSED: {len(bad)} of {len(names)} tile(s) differ between "
                  f"{hosts}")
            return 1
        print(f"[fleet] canary: {len(names)} tile(s) identical on {hosts}")
        return 0

    def canary_digests(self, host_name):
        """Return {tile: verdict digest} of the canary tiles pulled from one host."""
        d = os.path.join(self.local_run_dir, "canary", host_name)
        files = sorted(glob.glob(os.path.join(d, "*-vk.png")))
        return {os.path.basename(p): (dg[0] if dg else "undecodable")
                for p, dg in self.pb.digest_files(files).items()}

    # -- teardown --

    def stop_current(self, hs):
        """Stop a host's in-flight attempt (session and containers) on the way out."""
        try:
            hs.host.run_script(KILL_SCRIPT, [hs.engine, self.run_id, hs.root, 0,
                                             hs.current.base], timeout=120)
            print(f"[fleet] {hs.name}: stopped {hs.current.job.name}")
        except FleetError as e:
            print(f"[fleet] {hs.name}: stop failed: {e}")

    def teardown(self):
        """Stop in-flight attempts and delete this run's directories on every touched host."""
        self.span.release()
        for hs in self.hosts:
            if hs.current:
                self.stop_current(hs)
            if not hs.touched:
                continue
            keep = list(RUN_KEEP) if hs.host.local else []
            try:
                cp = hs.host.run_script(DELETE_SCRIPT, [hs.run_dir, *keep], timeout=600)
                ok = cp.returncode == 0
            except FleetError:
                ok = False
            print(f"[fleet] {hs.name}: " + (f"deleted {hs.run_dir}" if ok else
                                            f"{hs.run_dir} kept for clean (host unreachable)"))
        for e in os.listdir(self.local_run_dir):
            if e not in RUN_KEEP:
                p = os.path.join(self.local_run_dir, e)
                shutil.rmtree(p) if os.path.isdir(p) and not os.path.islink(p) else os.unlink(p)
        if self.golden_fd is not None:
            os.close(self.golden_fd)
            self.golden_fd = None

    # -- the whole run --

    def run(self):
        """Execute the run; return the exit code (1 > 64 > 0 over units)."""
        src_bytes = set_bytes(self.tree, source_set(self.tree, self.pb,
                                                    sorted({g for g, _t in self.units})))
        self.prepare_hosts(src_bytes)
        self.build_and_sync()
        self.jobs = self.make_jobs()
        self.run_jobs(self.jobs, on_done=self.unit_done)
        rc = worst_rc(j.result["rc"] if j.result else 1 for j in self.jobs if j.kind == "unit")
        if self.compare_canaries(self.jobs):
            rc = 1
        self.print_summary(rc)
        return rc

    def print_summary(self, rc):
        """Print each unit's host beside its verdict, then the run's verdict."""
        for j in self.jobs:
            r = j.result or {"rc": 1, "status": "no result", "host": "-"}
            verdict = {0: "PASS", EXIT_SKIP: "SKIP"}.get(r["rc"], "FAIL")
            print(f"[fleet] result {j.name}: {verdict} on {r['host']} (rc {r['rc']}, "
                  f"attempt {r.get('attempt', '-')}, {r.get('wall', '-')} s; {r['status']})")
        print(f"[fleet] run {self.run_id}: " + {0: "PASS", EXIT_SKIP: "SKIP"}.get(rc, "FAIL")
              + f" (exit {rc})")


# ---- add and calibrate: the calibration unit through run's machinery ----------

CALIB_DEADLINE_S = 4 * 3600         # one calibration leg, any slots
CALIB_LEG_TIMEOUT_S = 3600          # a sweep leg's, or an uncalibrated host's, harness leg timeout
IDLE_WAIT_S = 600                   # longest wait for the dev box to fall idle before a local leg


class Calibrator(Coordinator):
    """Legs of the calibration unit, placed on hosts through `run`'s preflight, sync, build and supervisor.

    plans maps a measured host to "sweep" (slots 1, 2, 4, ... up to its
    core count, stopped by sweep_should_stop) or to a fixed slots value.
    Each leg is one supervisor job (capture, then --verdict-only) whose done
    marker carries its wall time and the minimum MemAvailable sampled at
    1 Hz on the host. A local leg obeys the idle rule: a loaded leg is
    discarded and re-run, and the IDLE_RETRIES-th loaded leg at one slots
    value ends the calibration. legs holds each host's accepted legs.
    """

    def __init__(self, run_id, local_run_dir, measure, plans, local_host, dry_run=False):
        """Bind the measured hosts, their plans and the local host (the builder)."""
        a = argparse.Namespace(game=CALIBRATION_GAME, tier="presentation", all=False,
                               all_tiers=False, hosts="", no_local=False, pull="divergent")
        super().__init__(a, run_id, local_run_dir)
        self.measure, self.plans, self.local_host = measure, plans, local_host
        self.dry_run = dry_run
        self.legs = {h.name: [] for h in measure}
        self.loaded = {}
        self.infos = {}
        self.fit = {}
        self.seq = 0

    def canary_on(self):
        """False: calibration legs capture no canary."""
        return False

    def local_legs(self):
        """True when a leg runs on the local host."""
        return self.local_host.name in self.plans

    def planned_slots(self, hs):
        """Return the slots values a host's plan may run, in order."""
        plan = self.plans[hs.name]
        if plan != "sweep":
            return [plan]
        vals = sweep_values(int(hs.info["nproc"]))
        kept = [v for v in vals if v <= self.fit.get(hs.name, v)] or [1]
        if kept != vals and not getattr(self, "cap_shown", False):
            self.cap_shown = True
            print(f"[fleet] {hs.name}: sweep capped at slots {kept[-1]}: MemAvailable fits "
                  f"{self.fit[hs.name]} slot(s) at 2 GiB each")
        return kept

    def prepare_hosts(self, src_bytes):
        """Preflight every measured host and the builder, take the local lock when a local leg runs, clean, ship."""
        check_recipe(self.tree)
        sel = list(self.measure)
        if not self.local_legs():
            sel.append(self.local_host)
        results = preflight(sel, self.need_bytes(src_bytes), require_calibration=False)
        for h, i, reasons, _slots in results:
            self.infos[h.name] = i
            if self.dry_run or (h.local and not self.local_legs()):
                reasons = [r for r in reasons if not r.startswith("golden lock held")]
            for r in reasons:
                print(f"[fleet] {h.name}: preflight: {r}")
            if reasons:
                raise FleetError(f"{h.name}: preflight failed", EXIT_SKIP)
            plan = self.plans.get(h.name)
            fit = int(i.get("memavail_kib") or 0) // MEM_PER_SLOT_KIB
            if h.local:
                self.fit[h.name] = fit  # this machine's sweep stops at 2 GiB MemAvailable per slot
            if h.local and isinstance(plan, int) and fit < plan:
                raise FleetError(f"{h.name}: MemAvailable fits {fit} slot(s) at 2 GiB each, "
                                 f"under the recorded slots {plan}", EXIT_SKIP)
            hs = HostState(h, i, h.cfg.get("slots", 1), self.run_id)
            if h in self.measure:
                self.hosts.append(hs)
            if h.local:
                self.local_hs = hs
        builders = [hs for hs in self.hosts + [self.local_hs] if hs.host.cfg.get("build", True)]
        if not builders:
            raise FleetError("no measured host and not the local host may build (build = true)",
                             EXIT_SKIP)
        self.build_hs = max(builders, key=lambda hs: (hs.host.local, hs.speed))
        if self.dry_run:
            return
        if self.local_legs():
            self.golden_fd = try_golden_lock(self.local_host.fleet_root)
            if self.golden_fd is None:
                raise FleetError(f"{self.local_host.name}: golden lock taken since preflight",
                                 EXIT_SKIP)
        for hs in self.hosts:
            rc = clean_host(hs.host, "stale", held=hs.host.local)
            if rc != 0:
                raise FleetError(f"{hs.name}: start-of-run clean exited {rc}", EXIT_SKIP)
        for hs in {id(h): h for h in self.hosts + [self.build_hs]}.values():
            ship_image(hs.host, hs.info)

    def leg_timeout_s(self, job, hs):
        """Return a leg's harness timeout: a fixed leg's scaled by the recorded speed, else CALIB_LEG_TIMEOUT_S."""
        if self.plans.get(hs.name) != "sweep" and "speed" in hs.host.cfg:
            return leg_timeout(hs.speed)
        return CALIB_LEG_TIMEOUT_S

    def leg_job(self, hs, slots):
        """Return one calibration leg on a host at slots."""
        self.seq += 1
        return Job("unit", CALIBRATION_GAME, "presentation", pinned=hs.name, slots=slots,
                   label=f"calib-{hs.name}-s{slots}-{self.seq}")

    def print_plan(self):
        """Print the build host and every host's planned legs (dry run)."""
        print(f"[fleet] build host {self.build_hs.name}; sync set and snapshot as `run`; "
              f"unit {CALIBRATION_GAME} presentation")
        for hs in self.hosts:
            for s in self.planned_slots(hs):
                job = Job("unit", CALIBRATION_GAME, "presentation", pinned=hs.name, slots=s)
                print(f"[fleet] {hs.name} slots {s}: CORPUS_JOBS={s} "
                      f"LP_NUM_THREADS={lp_threads(hs.info, s)} "
                      f"PRESENTATION_LEG_TIMEOUT={self.leg_timeout_s(job, hs)} "
                      f"tests/harness/check_presentation_golden.sh --game {CALIBRATION_GAME}, "
                      f"then --verdict-only; MemAvailable 1 Hz"
                      + ("; idle rule" if hs.host.local else ""))
            if self.plans[hs.name] == "sweep":
                print(f"[fleet] {hs.name}: the sweep stops at the first step improving under "
                      f"{IMPROVEMENT_FLOOR * 100:.0f}% or a leg under 2 GiB MemAvailable")

    def launch(self, job, hs, deadline_s):
        """Wait for the dev box to fall idle before a local leg, then launch as `run` does."""
        if hs.host.local:
            job.load_before = self.wait_idle()
        return super().launch(job, hs, deadline_s)

    @staticmethod
    def wait_idle():
        """Wait up to IDLE_WAIT_S for the 1-minute load average to reach the idle bound; return it."""
        bound, t0, load = idle_bound(os.cpu_count()), time.monotonic(), loadavg1()
        if load > bound:
            print(f"[fleet] local: load {load:.2f} over the idle bound {bound:.2f}; waiting up "
                  f"to {IDLE_WAIT_S} s")
        while load > bound and time.monotonic() - t0 < IDLE_WAIT_S:
            time.sleep(15)
            load = loadavg1()
        return load

    def check_engaged(self, job, hs):
        """Refuse a leg whose verdict report compared other than every blessed row or lists a missing tile."""
        dst = os.path.join(self.local_run_dir, "calib", job.name)
        rsync_pull(hs.host, f"{job.result['unit_dir']}/cand", dst, names=["verdict-report.txt"])
        path = os.path.join(dst, "verdict-report.txt")
        text = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
        fields = parse_report(path)["fields"]
        rows = len(self.pb.load_manifest(CALIBRATION_GAME, "presentation").rows)
        missing = sum(1 for ln in text.splitlines() if ln.startswith("missing: "))
        if fields.get("compared") != str(rows) or missing:
            self.show_logs(job, hs)
            raise FleetError(f"{hs.name}: leg {job.name} compared {fields.get('compared') or 'none'} "
                             f"of {rows} blessed rows, {missing} missing; not a full unit")
        return fields.get("verdict", "")

    @staticmethod
    def show_logs(job, hs):
        """Print the tails of a failed leg's capture and verdict logs before teardown deletes them."""
        cp = hs.host.run_script('for f in "$1"/build.log "$1"/capture.log "$1"/verdict.log; do '
                                '[ -f "$f" ] && { echo "== $f"; tail -n 15 "$f"; }; done; exit 0',
                                [job.result.get("unit_dir", "")], timeout=60)
        for line in cp.stdout.splitlines():
            print(f"[fleet] {hs.name}: {line}")

    def leg_done(self, job, hs):
        """Accept or discard a finished leg; return the host's next leg, if any."""
        r = job.result
        if r["rc"] not in (0, 1) or r["status"] != "verdict":
            self.show_logs(job, hs)
            raise FleetError(f"{hs.name}: leg {job.name} ended rc {r['rc']} ({r['status']}); "
                             f"logs under {r.get('unit_dir', '?')} on {hs.name}")
        verdict = self.check_engaged(job, hs)
        leg = {"slots": job.slots, "wall_s": float(r["wall"]),
               "min_memavail_kib": r["min_memavail_kib"], "rc": r["rc"]}
        print(f"[fleet] {hs.name}: slots {job.slots}: {leg['wall_s']:.0f} s, min MemAvailable "
              f"{leg['min_memavail_kib'] / GIB_KIB:.2f} GiB, verdict {verdict}")
        if hs.host.local:
            leg.update(load_before=job.load_before, load_end=loadavg1())
            time.sleep(IDLE_SETTLE_S)
            leg["load_after"] = loadavg1()
            idle, why = leg_is_idle(leg, os.cpu_count())
            print(f"[fleet] {hs.name}: slots {job.slots}: {why}; "
                  + ("accepted" if idle else "loaded, discarded"))
            if not idle:
                key = (hs.name, job.slots)
                self.loaded[key] = self.loaded.get(key, 0) + 1
                if self.loaded[key] >= IDLE_RETRIES:
                    raise FleetError(f"{hs.name}: {IDLE_RETRIES} loaded legs at slots {job.slots}; "
                                     f"the machine is not idle", EXIT_SKIP)
                return [self.leg_job(hs, job.slots)]
        legs = self.legs[hs.name]
        legs.append(leg)
        if self.plans[hs.name] == "sweep" and not sweep_should_stop(legs):
            rest = [v for v in self.planned_slots(hs) if v > job.slots]
            if rest:
                return [self.leg_job(hs, rest[0])]
        return []

    def run(self):
        """Preflight, build and sync as `run`, then place every host's legs; return 0 or raise."""
        src_bytes = set_bytes(self.tree, source_set(self.tree, self.pb, [CALIBRATION_GAME]))
        self.prepare_hosts(src_bytes)
        if self.dry_run:
            self.print_plan()
            return 0
        self.build_and_sync()
        first = [self.leg_job(hs, self.planned_slots(hs)[0]) for hs in self.hosts]
        self.run_jobs(first, deadline=lambda _j, _hs: CALIB_DEADLINE_S, on_done=self.leg_done)
        for hs in self.hosts:
            if not self.legs[hs.name]:
                raise FleetError(f"{hs.name}: no accepted calibration leg "
                                 f"({hs.dropped or 'no result'})")
        return 0


def run_calibration(local_host, measure, plans, dry_run):
    """Run a Calibrator under drive() (directly under --dry-run, which writes nothing); return (rc, calibrator)."""
    run_id = new_run_id()
    run_dir = os.path.join(expand_root(local_host.fleet_root), "runs", run_id)
    make = lambda: Calibrator(run_id, run_dir, measure, plans, local_host, dry_run)  # noqa: E731
    if dry_run:
        co = make()
        return co.run(), co
    print(f"[fleet] calibration run {run_id}: {run_dir}")
    return drive(run_id, run_dir, make)


def check_recipe(tree):
    """Refuse (64) when the local capture image was built from another recipe than the tree's."""
    engine = local_engine()
    if not engine:
        raise FleetError("no local container engine", EXIT_SKIP)
    label = image_inspect(engine, local_image(),
                          '{{index .Labels "org.openpete.capture-recipe"}}')
    want = sha256_path(os.path.join(tree, "docs", "linux-capture.Dockerfile"))
    if label != want:
        raise FleetError(f"local image {local_image()} was built from recipe "
                         f"{label[:12] or 'none'}, the tree's is {want[:12]}; rebuild it with "
                         f"scripts/golden-capture-container.sh image", EXIT_SKIP)


def worst_rc(rcs):
    """Return the worst exit code in the order 1 > 64 > 0."""
    rcs = list(rcs)
    if any(r not in (0, EXIT_SKIP) for r in rcs):
        return 1
    return EXIT_SKIP if EXIT_SKIP in rcs or not rcs else 0


def first_hex(value):
    """Return the leading sha256 of a `<sha> <path>` report value, or ''."""
    tok = value.split()[0] if value.split() else ""
    return tok if re.fullmatch(r"[0-9a-f]{64}", tok) else ""


def parse_report(path):
    """Parse verdict-report.txt into fields, divergent tiles and identity-pair members."""
    out = {"fields": {}, "divergent": [], "identity": []}
    try:
        text = open(path, encoding="utf-8").read()
    except OSError:
        return out
    for line in text.splitlines():
        key, _sep, val = line.partition(": ")
        if key == "divergent":
            out["divergent"].append(val.split()[0])
        elif key.startswith("identity-"):
            out["identity"] += [t for t in val.split()[:2] if t.endswith(".png")]
        elif key:
            out["fields"].setdefault(key, val.strip())
    out["identity"] = sorted(set(out["identity"]))
    return out


# ---- run: entry points --------------------------------------------------------

def local_fleet_root():
    """Return the local host's expanded fleet_root (the default without a local host)."""
    try:
        _p, hosts = load_hosts()
    except FleetError:
        hosts = {}
    root = next((c.get("fleet_root", DEFAULT_FLEET_ROOT) for c in hosts.values()
                 if c["transport"] == "local"), DEFAULT_FLEET_ROOT)
    return expand_root(root)


def new_run_id():
    """Return a run id: UTC time plus four random hex digits."""
    return time.strftime("%Y%m%d-%H%M%S", time.gmtime()) + "-" + os.urandom(2).hex()


def raise_interrupted(signum, _frame):
    """Turn a termination signal into an exception so teardown runs."""
    raise Interrupted(f"interrupted by signal {signum}", 1)


def drive(run_id, run_dir, factory):
    """Run factory().run() under the run directory's active lock; teardown and done marker on every exit.

    Returns (exit code, coordinator or None). SIGTERM, SIGINT and SIGHUP
    become Interrupted so teardown stops in-flight attempts and deletes the
    run's directories; the done marker's first line is `rc=N`.
    """
    os.makedirs(run_dir, exist_ok=True)
    active = os.open(os.path.join(run_dir, "active.lock"), os.O_WRONLY | os.O_CREAT, 0o644)
    fcntl.flock(active, fcntl.LOCK_SH)
    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, raise_interrupted)
    rc, why, co = 1, "", None
    try:
        co = factory()
        rc = co.run()
    except FleetError as e:
        rc, why = e.code, str(e)
        print(f"[fleet] {'SKIP' if rc == EXIT_SKIP else 'FAIL'}: {why}")
    except Exception as e:  # the done marker is written whatever ends the run
        rc, why = 1, f"{type(e).__name__}: {e}"
        import traceback
        traceback.print_exc()
    finally:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        if co is not None:
            co.teardown()
        with open(os.path.join(run_dir, "done.tmp"), "w") as f:
            f.write(f"rc={rc}\nrun={run_id}\n" + (f"reason={why}\n" if why else ""))
        os.replace(os.path.join(run_dir, "done.tmp"), os.path.join(run_dir, "done"))
        os.close(active)
    return rc, co


def run_foreground(a):
    """Run the coordinator in this process under drive()."""
    run_dir = os.path.join(local_fleet_root(), "runs", a.run_id)
    rc, _co = drive(a.run_id, run_dir, lambda: Coordinator(a, a.run_id, run_dir))
    return rc


def cmd_run(a):
    """Detach a coordinator (setsid) and print its log and done marker; --wait follows it."""
    if a.foreground:
        return run_foreground(a)
    select_units(a)
    load_hosts()
    a.run_id = new_run_id()
    run_dir = os.path.join(local_fleet_root(), "runs", a.run_id)
    os.makedirs(run_dir)
    log = os.path.join(run_dir, "coordinator.log")
    argv = [sys.executable, os.path.abspath(__file__), "run", "--foreground",
            "--run-id", a.run_id, *[x for x in sys.argv[2:] if x != "--wait"]]
    with open(log, "w") as f:
        subprocess.Popen(argv, cwd=script_repo(), stdout=f, stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, start_new_session=True)
    done = os.path.join(run_dir, "done")
    print(f"[fleet] run {a.run_id} detached\n  log:  {log}\n  done: {done}")
    if not a.wait:
        return 0
    while not os.path.exists(done):
        time.sleep(5)
    sys.stdout.write(open(log, encoding="utf-8", errors="replace").read())
    return int(parse_kv(open(done).read()).get("rc", 1))


def main(argv=None):
    """Parse the command line and dispatch to a subcommand."""
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("status")
    p.add_argument("--hosts", default="")
    p.set_defaults(fn=cmd_status)
    p = sub.add_parser("preflight")
    p.add_argument("--hosts", default="")
    p.add_argument("--need-gib", type=float, default=0.0)
    p.add_argument("--no-calibration", action="store_true",
                   help="do not drop an uncalibrated host")
    p.set_defaults(fn=cmd_preflight)
    p = sub.add_parser("ship-image")
    p.add_argument("--hosts", required=True)
    p.add_argument("--dry-run", action="store_true")
    p.set_defaults(fn=cmd_ship_image)
    p = sub.add_parser("add")
    p.add_argument("name")
    p.add_argument("--ssh")
    p.add_argument("--engine", choices=("podman", "docker"))
    p.add_argument("--fleet-root")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--timings")
    p.set_defaults(fn=cmd_add)
    p = sub.add_parser("calibrate")
    p.add_argument("--hosts", default="")
    p.add_argument("--dry-run", action="store_true")
    p.set_defaults(fn=cmd_calibrate)
    p = sub.add_parser("run")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--game", default="spyro-1", choices=GAMES)
    g.add_argument("--all", action="store_true", help="every game")
    t = p.add_mutually_exclusive_group()
    t.add_argument("--tier", default="presentation", choices=tuple(TIER_GAMES))
    t.add_argument("--all-tiers", action="store_true", help="presentation and dragons")
    h = p.add_mutually_exclusive_group()
    h.add_argument("--hosts", default="")
    h.add_argument("--no-local", action="store_true", help="leave the local host out")
    p.add_argument("--pull", choices=("divergent", "all"), default="divergent")
    p.add_argument("--wait", action="store_true", help="follow the detached run to its end")
    p.add_argument("--foreground", action="store_true", help=argparse.SUPPRESS)
    p.add_argument("--run-id", help=argparse.SUPPRESS)
    p.set_defaults(fn=cmd_run)
    p = sub.add_parser("clean")
    p.add_argument("--hosts", default="")
    p.add_argument("--tree", help="remove only the trees this coordinator keyed on PATH")
    p.add_argument("--all", action="store_true",
                   help="remove every fleet file, fleet container and the :fleet tag")
    p.set_defaults(fn=cmd_clean)
    a = ap.parse_args(argv)
    sys.stdout.reconfigure(line_buffering=True)
    try:
        return a.fn(a)
    except FleetError as e:
        msg = str(e)
        if e.code == EXIT_SKIP and not msg.startswith("SKIP"):
            msg = "SKIP: " + msg
        print(f"[fleet] {msg}", file=sys.stderr)
        return e.code


if __name__ == "__main__":
    sys.exit(main())
