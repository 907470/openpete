"""child_env — environment knobs that actually reach the child process.

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
import os


def child_env(knobs, base=None):
    """os.environ (or `base`) plus `knobs`, with WSLENV extended to match."""
    env = dict(os.environ if base is None else base)
    if not knobs:
        return env
    env.update({k: str(v) for k, v in knobs.items()})
    names = ":".join(f"{k}/w" for k in knobs)
    env["WSLENV"] = f"{env['WSLENV']}:{names}" if env.get("WSLENV") else names
    return env
