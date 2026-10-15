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
import shutil
import tempfile


def child_env(knobs, base=None):
    """os.environ (or `base`) plus `knobs`, with WSLENV extended to match."""
    env = dict(os.environ if base is None else base)
    if not knobs:
        return env
    env.update({k: str(v) for k, v in knobs.items()})
    names = ":".join(f"{k}/w" for k in knobs)
    env["WSLENV"] = f"{env['WSLENV']}:{names}" if env.get("WSLENV") else names
    return env


def scratch_cards():
    """Export OPENPETE_CARD_DIR as a fresh card directory owned by this process.

    The Python side of scripts/lib/scratch_cards.sh, same parent directory and
    naming (<pid>.<random> under $TMPDIR/openpete-harness-cards): without it a
    run given no card flag opens the player's cards in the per-user state
    directory. Every child this process starts inherits the variable and boots
    on two freshly formatted cards; --card1, --card2 and --card-dir still beat
    it. An inherited directory under the parent that exists is kept (the
    caller is a harness that already made one). The directory is removed at
    exit; directories whose owning process is gone are removed on each call.
    A Windows exe launched from WSL does not receive the variable.
    Returns the directory.
    """
    parent = os.path.join(os.environ.get("TMPDIR") or "/tmp", "openpete-harness-cards")
    cur = os.environ.get("OPENPETE_CARD_DIR", "")
    if cur.startswith(parent + os.sep) and os.path.isdir(cur):
        return cur
    os.makedirs(parent, exist_ok=True)
    for name in os.listdir(parent):
        pid = name.split(".", 1)[0]
        if pid.isdigit() and not os.path.isdir(f"/proc/{pid}"):
            shutil.rmtree(os.path.join(parent, name), ignore_errors=True)
    d = tempfile.mkdtemp(prefix=f"{os.getpid()}.", dir=parent)
    os.environ["OPENPETE_CARD_DIR"] = d
    atexit.register(shutil.rmtree, d, True)
    return d
