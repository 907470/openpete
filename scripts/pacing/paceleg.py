"""paceleg — running one paced leg of the game, and finding the binary.

Every pacing harness needs the same three things: the right binary for the
platform, a log destroyed before the run rather than after it, and knobs that
actually reach the child. They live here so a second harness cannot drift into
a third answer for any of them — in particular the WSLENV forwarding, which
fails silently and yields a real, internally consistent measurement of a
configuration nobody asked for.
"""
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from hostenv import child_env                                  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
VBL_HZ = 2250000 / 37609


def exe_for(platform):
    if platform == "windows":
        p = REPO / "build/windows-mingw/games/spyro-1/openpete-spyro1.exe"
    else:
        p = REPO / "build/linux-release/games/spyro-1/openpete-spyro1"
    if not p.exists():
        sys.exit(f"no binary at {p} — build the preset first")
    return p


def run_leg(exe, disc, name, flags, seconds, out_dir, headless, extra,
            knobs=None):
    """Run one leg for `seconds` of paced time; return its log path or None."""
    log = out_dir / f"{name}.pacelog"
    # Every harness destroys its own history before it runs: a stale log that
    # survives a failed leg reads exactly like a successful one.
    log.unlink(missing_ok=True)
    # The end-of-run summaries (pace, present-thread, subtick-view) only exist
    # on the child's console, and a harness that has to re-run the game to read
    # one is a harness nobody re-reads. Same destroy-first rule.
    console = out_dir / f"{name}.stdout"
    console.unlink(missing_ok=True)
    vbls = int(seconds * VBL_HZ)
    cmd = [str(exe), "--disc", str(disc), "--frame-limit", str(vbls),
           "--pace-log", str(log)] + list(flags) + list(extra)
    env_knobs = dict(knobs or {})
    if headless:
        cmd.append("--headless")
        env_knobs["OPENPETE_FORCE_PACING"] = "1"  # headless is unpaced otherwise
    t0 = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True,
                       env=child_env(env_knobs),
                       timeout=seconds * 4 + 300)
    console.write_text((r.stdout or "") + (r.stderr or ""))
    if r.returncode != 0 or not log.exists():
        tail = (r.stderr or r.stdout or "")[-400:]
        print(f"  {name}: FAILED (rc={r.returncode}) {tail}", file=sys.stderr)
        return None
    print(f"  {name}: {time.time()-t0:.0f}s wall, {log.stat().st_size//1024} KiB")
    return log
