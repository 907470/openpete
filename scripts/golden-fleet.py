#!/usr/bin/env python3
"""golden-fleet.py -- place presentation-golden units on a fleet of hosts.

    golden-fleet.py status [--hosts a,b]
    golden-fleet.py preflight [--hosts a,b] [--need-gib N] [--no-calibration]
    golden-fleet.py ship-image --hosts a,b
    golden-fleet.py add local [--dry-run] [--timings FILE]
    golden-fleet.py add <name> --ssh user@host [--engine podman|docker]
                    [--fleet-root DIR] [--dry-run] [--timings FILE]

Host file. $OPENPETE_FLEET_HOSTS, else `<main checkout>/../<planning checkout>/
fleet-hosts.toml`, the main checkout resolved through `git rev-parse
--git-common-dir` so a worktree resolves the same file. One table per host:

    [hosts.<name>]
    transport = "local" | "ssh"     # required; `local` is the coordinator
    ssh = "user@host"               # required for transport "ssh"
    fleet_root = "~/.cache/openpete-fleet"   # every fleet file on the host
    engine = "podman" | "docker"    # default: the first found on the host
    slots = N                       # CORPUS_JOBS on the host, measured by add
    speed = F                       # throughput relative to local (1.00)
    calibrated = "YYYY-MM-DD governor=G cpu=MODEL"
    enabled = true                  # false: never used
    default = true                  # in every run unless opted out
    build = true                    # may build the engine for a run
    nice = 19                       # CPU priority of units on the host

Only `add` writes the file: it replaces the named host's table and keeps
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

add: preflights the host (calibration not required), ships the image,
then sweeps the spyro-2 presentation unit at slots 1, 2, 4, ... up to the
host's core count while sampling MemAvailable at 1 Hz. The rule: slots is
the smallest sweep value past which wall-clock improves by less than 10%,
lowered while the minimum MemAvailable during its leg is under 2 GiB. For
a remote host, speed = t_local / t_host, t_local being the same unit on
this machine at the local host's recorded slots with this machine idle
(the 1-minute load average at most max(1, 0.1 x cores) before the leg,
and, 90 s after it ends, at most that bound plus a quarter of the load at
its end; a loaded measurement is discarded and retried up to 3 times).
`add local` records speed 1.00. The host's table is then written with
slots, speed and calibrated (date, governor, CPU model).

--timings FILE replaces the measurement with recorded legs (JSON):

    {"cores": 8,
     "sweep": [{"slots": 1, "wall_s": 1500, "min_memavail_kib": 9000000}, ...],
     "local": [{"slots": 4, "wall_s": 270, "load_before": 0.4,
                "load_end": 22.0, "load_after": 5.1}, ...]}

and ships no image; with --dry-run nothing is written and every command a
measured run would execute is printed with the decision.
"""

import argparse
import datetime
import fcntl
import json
import os
import re
import shlex
import subprocess
import sys
import threading
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

HOST_KEYS = {
    "transport": str, "ssh": str, "fleet_root": str, "engine": str,
    "slots": int, "speed": float, "calibrated": str, "enabled": bool,
    "default": bool, "build": bool, "nice": int,
}
KEY_ORDER = ["transport", "ssh", "fleet_root", "engine", "slots", "speed",
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
    """Return <main checkout>/../<planning checkout>/fleet-hosts.toml via the common git dir."""
    common = subprocess.run(
        ["git", "-C", script_repo(), "rev-parse", "--path-format=absolute",
         "--git-common-dir"], capture_output=True, text=True, check=True).stdout.strip()
    main = os.path.dirname(common.rstrip("/"))
    return os.path.normpath(os.path.join(main, "..", "notes", "fleet-hosts.toml"))


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
printf '%s\n' "$id" >"$root/image.id.tmp"
mv "$root/image.id.tmp" "$root/image.id"
'''


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
        save = [engine, "save", local_id]
        load = host.stream_command([host_engine, "load"])
        print(f"[fleet] {host.name}: {shlex.join(save)} | {shlex.join(load)}")
        if not dry_run:
            t0 = time.monotonic()
            src = subprocess.Popen(save, stdout=subprocess.PIPE)
            dst = subprocess.run(load, stdin=src.stdout, capture_output=True, text=True)
            src.stdout.close()
            if src.wait() != 0 or dst.returncode != 0:
                raise FleetError(f"{host.name}: image transfer failed (save {src.returncode}, "
                                 f"load {dst.returncode}): {dst.stderr.strip()[-300:]}")
            print(f"[fleet] {host.name}: shipped in {time.monotonic() - t0:.0f} s")
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


class MemSampler(threading.Thread):
    """Samples local MemAvailable at 1 Hz and keeps the minimum."""

    def __init__(self):
        """Start with no minimum and a stop event."""
        super().__init__(daemon=True)
        self.min_kib = None
        self.stop = threading.Event()

    def run(self):
        """Read /proc/meminfo once a second until stopped."""
        while not self.stop.is_set():
            kib = read_memavail_kib()
            self.min_kib = kib if self.min_kib is None else min(self.min_kib, kib)
            self.stop.wait(1.0)


def read_memavail_kib():
    """Return the local MemAvailable in KiB."""
    with open("/proc/meminfo") as f:
        for line in f:
            if line.startswith("MemAvailable:"):
                return int(line.split()[1])
    raise FleetError("no MemAvailable in /proc/meminfo")


def loadavg1():
    """Return the local 1-minute load average."""
    return os.getloadavg()[0]


def local_leg_env(root, slots):
    """Return the environment of a local calibration leg under <root>/calib."""
    calib = os.path.join(root, "calib")
    env = dict(os.environ)
    env.update({
        "CORPUS_JOBS": str(slots),
        "OPENPETE_CAPTURE_IMAGE": IMAGE_TAG,
        "OPENPETE_FLEET_ROOT": root,
        "OPENPETE_GOLDEN_LOCK_HELD": os.path.join(root, "golden.lock"),
        "OPENPETE_GOLDEN_REF_DIR": os.path.join(calib, "store"),
        "PRESENTATION_GOLDEN_OUT": os.path.join(calib, "out"),
    })
    return env


def local_leg_argv(nice):
    """Return the argv of one local calibration leg (the calibration unit's harness)."""
    return ["nice", "-n", str(nice), "tests/harness/check_presentation_golden.sh",
            "--game", CALIBRATION_GAME]


def run_local_leg(tree, root, slots, nice):
    """Run the calibration unit locally at `slots`; return the leg's measurements."""
    env = local_leg_env(root, slots)
    out = env["PRESENTATION_GOLDEN_OUT"]
    subprocess.run(["rm", "-rf", out], check=True)
    sampler = MemSampler()
    load_before = loadavg1()
    sampler.start()
    t0 = time.monotonic()
    rc = subprocess.run(local_leg_argv(nice), cwd=tree, env=env).returncode
    wall = time.monotonic() - t0
    sampler.stop.set()
    sampler.join()
    load_end = loadavg1()
    report = os.path.join(out, "verdict-report.txt")
    if rc not in (0, 1) or not os.path.exists(report):
        raise FleetError(f"calibration leg at slots {slots} exited {rc} "
                         f"({'no' if not os.path.exists(report) else 'with'} {report})")
    time.sleep(IDLE_SETTLE_S)
    return {"slots": slots, "wall_s": wall, "rc": rc, "min_memavail_kib": sampler.min_kib,
            "load_before": load_before, "load_end": load_end, "load_after": loadavg1()}


def measure_sweep(host, info, tree, dry_run):
    """Run or print the slots sweep on a host; return legs (empty under dry-run)."""
    cores = int(info["nproc"])
    root = info["root"]
    legs = []
    for s in sweep_values(cores):
        env = local_leg_env(root, s)
        shown = " ".join(f"{k}={shlex.quote(env[k])}" for k in (
            "CORPUS_JOBS", "OPENPETE_CAPTURE_IMAGE", "OPENPETE_FLEET_ROOT",
            "OPENPETE_GOLDEN_LOCK_HELD", "OPENPETE_GOLDEN_REF_DIR", "PRESENTATION_GOLDEN_OUT"))
        if not host.local:
            print(f"[fleet] {host.name} slots {s}: needs the snapshot sync and remote "
                  f"unit supervisor that `run` provides; leg not runnable yet")
            continue
        print(f"[fleet] {host.name} slots {s}: (cd {tree} && {shown} "
              f"{shlex.join(local_leg_argv(host.cfg.get('nice', 19)))}) + MemAvailable 1 Hz")
        if dry_run:
            continue
        legs.append(run_local_leg(tree, root, s, host.cfg.get("nice", 19)))
        print(f"[fleet] slots {s}: {legs[-1]['wall_s']:.0f} s, min MemAvailable "
              f"{legs[-1]['min_memavail_kib'] / GIB_KIB:.2f} GiB, verdict rc {legs[-1]['rc']}")
        if sweep_should_stop(legs):
            break
    return legs


def measure_t_local(local_host, tree, dry_run):
    """Run or print the local reference legs at the local host's slots until one is idle."""
    root = expand_root(local_host.fleet_root)
    slots = local_host.cfg.get("slots")
    if slots is None:
        raise FleetError("the local host has no recorded slots; run `add local` first")
    cores = os.cpu_count()
    print(f"[fleet] t_local: {CALIBRATION_GAME} at local slots {slots}, idle rule "
          f"load <= {idle_bound(cores):.2f}, re-checked {IDLE_SETTLE_S} s after; "
          f"up to {IDLE_RETRIES} attempts")
    if dry_run:
        return []
    legs = []
    for _ in range(IDLE_RETRIES):
        legs.append(run_local_leg(tree, root, slots, local_host.cfg.get("nice", 19)))
        if leg_is_idle(legs[-1], cores)[0]:
            break
    return legs


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


def cmd_add(a):
    """Preflight, ship, sweep and record a host's slots, speed and calibration."""
    path, hosts = load_hosts(required=False)
    if a.name != "local" and hosts.get(a.name, {}).get("transport") == "local":
        raise FleetError(f"{a.name} is the local host; use `add local`")
    if a.name == "local":
        other = [n for n, c in hosts.items() if c["transport"] == "local" and n != "local"]
        if other:
            raise FleetError(f"the host file already names a local host: {other[0]}")
    cfg = new_host_cfg(a, hosts.get(a.name))
    host = Host(a.name, cfg)
    timings = json.load(open(a.timings)) if a.timings else None
    measuring = timings is None

    (_h, info, reasons, _s), = preflight([host], need_bytes=CALIB_NEED_BYTES,
                                         require_calibration=False)
    if a.dry_run or not measuring:
        reasons = [r for r in reasons if not r.startswith("golden lock held")]
    for r in reasons:
        print(f"[fleet] {host.name}: preflight: {r}")
    if reasons:
        raise FleetError(f"{host.name}: preflight failed", EXIT_SKIP)

    tree = script_repo()
    if measuring:
        if not host.local and not a.dry_run:
            raise FleetError(f"{host.name}: a measured remote sweep needs the snapshot sync "
                             f"and unit supervisor that `run` provides; pass --timings FILE "
                             f"with legs measured by hand", EXIT_SKIP)
        lock_fd = None
        if not a.dry_run:
            lock_fd = try_golden_lock(info["root"])
            if lock_fd is None:
                raise FleetError(f"{host.name}: golden lock busy", EXIT_SKIP)
        ship_image(host, info, dry_run=a.dry_run)
        legs = measure_sweep(host, info, tree, a.dry_run)
        local_legs = []
        if not host.local:
            local_host = next((Host(n, c) for n, c in hosts.items()
                               if c["transport"] == "local"), None)
            if local_host is None:
                raise FleetError("no local host in the host file; run `add local` first")
            local_legs = measure_t_local(local_host, tree, a.dry_run)
        cores = int(info["nproc"])
        if lock_fd is not None:
            os.close(lock_fd)
        if a.dry_run:
            print("[fleet] dry run: no legs measured; pass --timings FILE to apply the rules")
            return 0
    else:
        legs, local_legs = timings["sweep"], timings.get("local", [])
        cores = int(timings.get("cores") or info["nproc"])
        print(f"[fleet] legs from {a.timings} (cores {cores}); sweep values "
              f"{sweep_values(cores)}")

    slots, t_host, why = pick_slots(legs)
    for w in why:
        print(f"[fleet] rule: {w}")
    if host.local:
        speed = 1.0
    else:
        t_local = pick_t_local(local_legs, os.cpu_count())
        speed = t_local / t_host
        print(f"[fleet] speed = t_local {t_local:.0f} s / t_host {t_host:.0f} s = {speed:.2f}")
    date = datetime.date.today().isoformat()
    cfg["slots"], cfg["speed"] = slots, round(speed, 2)
    cfg["calibrated"] = f"{date} governor={info['governor']} cpu={info['cpu']}"
    print(f"[fleet] {'would write' if a.dry_run else 'writing'} {path}:")
    sys.stdout.write(format_table(a.name, cfg))
    if not a.dry_run:
        write_host_table(path, a.name, cfg)
    return 0


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
