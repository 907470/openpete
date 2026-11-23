"""child_env - environment knobs that actually reach the child process.

Two ways a knob silently fails to arrive, both of which yield a real,
internally consistent result about a configuration nobody asked for:

  - building an env dict and never passing it to subprocess, so the child
    inherits the parent's environment without the knob;
  - passing it to a Windows .exe launched from WSL, where interop forwards
    only the names listed in WSLENV.

Neither raises. The exe starts, reads the variable as unset, and runs the
default. scripts/fences/wslenv_guard.py refuses a script that sets an
OPENPETE_* knob near a Windows launch without going through here.
"""
import atexit
import os
import re
import shutil
import subprocess
import tempfile


# Knobs a gate harness exports that cross with every forward, with their
# WSLENV flags (/p translates a path).
FORWARDED = {"OPENPETE_THREAD_ROLE_FATAL": "/w", "OPENPETE_THREAD_ROLE_REPORT": "/wp",
             "OPENPETE_CARD_GUARD": "/w", "OPENPETE_CARD_DIR": "/wp",
             "OPENPETE_SANDBOX": "/wp"}


def child_env(knobs, base=None):
    """os.environ (or `base`) plus `knobs`, with WSLENV extended to match;
    the FORWARDED knobs set in the environment cross too."""
    env = dict(os.environ if base is None else base)
    knobs = knobs or {}
    env.update({k: str(v) for k, v in knobs.items()})
    names = ":".join([f"{k}/w" for k in knobs]
                     + [k + f for k, f in FORWARDED.items() if env.get(k) and k not in knobs])
    if not names:
        return env
    env["WSLENV"] = f"{env['WSLENV']}:{names}" if env.get("WSLENV") else names
    return env


def sandbox_cache_dir():
    """The shared pipeline-cache directory linked into every sandbox root, created.

    $OPENPETE_SANDBOX_CACHE, else $XDG_CACHE_HOME/openpete-harness/cache
    (~/.cache/openpete-harness/cache); the same directory
    scripts/lib/sandbox.sh links.
    """
    c = os.environ.get("OPENPETE_SANDBOX_CACHE") or os.path.join(
        os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"),
        "openpete-harness", "cache")
    os.makedirs(c, exist_ok=True)
    return c


def sandbox_link_cache(root):
    """Make root/state/openpete and link its cache to the shared directory; a
    root on a Windows drive (/mnt/<letter>/) gets a plain directory."""
    state = os.path.join(root, "state", "openpete")
    os.makedirs(state, exist_ok=True)
    cache = os.path.join(state, "cache")
    if os.path.lexists(cache):
        return
    if re.match(r"/mnt/[a-z]/", root):
        os.makedirs(cache, exist_ok=True)
    else:
        os.symlink(sandbox_cache_dir(), cache)


def sandbox_xdg(root):
    """The XDG config, state and data bases under root/xdg, as a dict; root/xdg/state/openpete/cache is linked
    to the shared pipeline cache.

    A binary that predates OPENPETE_SANDBOX resolves its per-user directories from these, so it writes inside
    the root too; they lie apart from root/{config,state,data} because a sandboxed build refuses a write path
    inside the directories its XDG variables name. Pass it in one launch's environment, never into
    os.environ: ccache, git, podman and the harness's own stores read the same variables. The layout
    scripts/lib/sandbox.sh op_sandbox_xdg makes.
    """
    xdg = os.path.join(root, "xdg")
    sandbox_link_cache(xdg)
    env = {"XDG_CONFIG_HOME": os.path.join(xdg, "config"), "XDG_STATE_HOME": os.path.join(xdg, "state"),
           "XDG_DATA_HOME": os.path.join(xdg, "data")}
    for d in env.values():
        os.makedirs(d, exist_ok=True)
    return env


def timing_gate(label=None):
    """Block while another job holds the timing window (scripts/bench/timing_lock.py), then hold it shut.

    Returns when the window is free or this process runs inside it (its
    OPENPETE_TIMING_TOKEN is the holder's). Never returns while another
    job's window is open: with OPENPETE_TIMING_GATE_TIMEOUT=<seconds> it
    exits 75 at the limit. Outside a window this process then holds it shut
    (timing_lock.gate) until it exits, yielding at a later call while a
    timing job waits for it.
    """
    import sys
    bench = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bench")
    if bench not in sys.path:
        sys.path.insert(0, bench)
    import timing_lock
    t = os.environ.get("OPENPETE_TIMING_GATE_TIMEOUT")
    if not timing_lock.gate(label=label or os.path.basename(sys.argv[0]),
                            timeout=float(t) if t else None):
        sys.exit(timing_lock.EXIT_TIMEOUT)


def sandbox():
    """Export OPENPETE_SANDBOX as a fresh root owned by this process; returns it.

    Each call first waits out another job's timing window (timing_gate), so
    sandbox_run_root() gates every launch that takes a run root.

    The Python side of scripts/lib/sandbox.sh, same parent and naming
    (<pid>.<random> under $OPENPETE_SANDBOX_PARENT, else
    $TMPDIR/openpete-harness-sandbox): under the root the game resolves every
    per-user directory (config, logs, hang files, bug bundles, cards,
    savestates, screenshots, recordings), so no run touches the player's
    files, and it refuses to start when a write path lies in a real one.
    OPENPETE_CARD_DIR is exported as <root>/cards. An inherited root under the
    parent that exists is kept. The root is removed at exit; roots whose
    owning process is gone are removed on each call. A Windows exe launched
    from WSL receives it through child_env.
    """
    timing_gate()
    parent = os.environ.get("OPENPETE_SANDBOX_PARENT") or os.path.join(
        os.environ.get("TMPDIR") or "/tmp", "openpete-harness-sandbox")
    cur = os.environ.get("OPENPETE_SANDBOX", "")
    if cur.startswith(parent + os.sep) and os.path.isdir(cur):
        os.environ.setdefault("OPENPETE_CARD_DIR", os.path.join(cur, "cards"))
        os.makedirs(os.environ["OPENPETE_CARD_DIR"], exist_ok=True)
        return cur
    os.makedirs(parent, exist_ok=True)
    for name in os.listdir(parent):
        pid = name.split(".", 1)[0]
        if pid.isdigit() and not os.path.isdir(f"/proc/{pid}"):
            shutil.rmtree(os.path.join(parent, name), ignore_errors=True)
    d = tempfile.mkdtemp(prefix=f"{os.getpid()}.", dir=parent)
    sandbox_link_cache(d)
    os.makedirs(os.path.join(d, "cards"), exist_ok=True)
    os.environ["OPENPETE_SANDBOX"] = d
    os.environ["OPENPETE_CARD_DIR"] = os.path.join(d, "cards")
    atexit.register(shutil.rmtree, d, True)
    return d


def sandbox_run_root():
    """A fresh per-run root under OPENPETE_SANDBOX (sandbox() first), with the
    shared cache linked in and an empty cards directory; removed with the
    parent root at exit."""
    r = tempfile.mkdtemp(prefix="run.", dir=sandbox())
    sandbox_link_cache(r)
    os.makedirs(os.path.join(r, "cards"), exist_ok=True)
    return r


def scratch_cards():
    """The scratch memory cards of a harness, now its sandbox: sandbox(), then
    the card directory OPENPETE_CARD_DIR names (<root>/cards). Returns it."""
    sandbox()
    return os.environ["OPENPETE_CARD_DIR"]


LVP_ICD = "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"


def sw_render_env(env):
    """Pin `env`'s GL and Vulkan to Mesa's CPU rasterizers; returns `env`.

    The Python side of scripts/lib/swrender.sh, same selection:
    VK_ICD_FILENAMES names the lavapipe ICD when installed,
    LIBGL_ALWAYS_SOFTWARE=1 keeps the GL loader off hardware DRI drivers,
    GALLIUM_DRIVER=llvmpipe (unless already set) keeps swrast_dri.so off d3d12
    on WSL, and MESA_LOADER_DRIVER_OVERRIDE is removed (Mesa ships no
    llvmpipe_dri.so, so that value selects nothing).
    """
    if os.path.exists(LVP_ICD):
        env["VK_ICD_FILENAMES"] = LVP_ICD
    env["LIBGL_ALWAYS_SOFTWARE"] = "1"
    env.setdefault("GALLIUM_DRIVER", "llvmpipe")
    env.pop("MESA_LOADER_DRIVER_OVERRIDE", None)
    return env


XVFB_SH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "xvfb.sh")


def xvfb_prefix(tag, screen, env=None):
    """The argv prefix that runs a windowed child on a private Xvfb display.

    The Python side of scripts/lib/xvfb.sh, which holds the policy (a private
    Xvfb display with WAYLAND_DISPLAY removed and SDL_VIDEODRIVER=x11, or the
    session's display under OPENPETE_HARNESS_REAL_DISPLAY=1). Returns
    (prefix, None), the prefix to put before the binary's argv, or
    (None, line) with the SKIP line when no display can be given.
    """
    r = subprocess.run(["bash", XVFB_SH, "--check", tag, screen], env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    if r.returncode != 0:
        return None, r.stdout.strip() or f"{tag}: SKIP (no display)"
    return ["bash", XVFB_SH, tag, screen, "--"], None
