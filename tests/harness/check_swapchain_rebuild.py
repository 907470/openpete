#!/usr/bin/env python3
"""Swapchain rebuilds on a window the driver keeps bound: every resize rebuilds the swapchain, and presents
continue after the last one.

One windowed run under a fresh virtual display (xvfb-run, the minimal window manager tests/harness/xvfb_wm.c,
lavapipe; check_launcher.launch) with OPENPETE_VK_FAULT set to a window-held mode, which models a driver that
keeps the window bound to Vulkan after vkDestroySwapchainKHR: a vkCreateSwapchainKHR on the bound window
returns VK_ERROR_NATIVE_WINDOW_IN_USE_KHR. The binding is released by a new VkSurfaceKHR, and in mode
window-held also by a creation that names the window's live swapchain as oldSwapchain
(fault_window_held_mode in core/rhi/rhi_vulkan.c).

Cases:
  launcher  the launcher's window, resized RESIZES times through OPENPETE_LAUNCHER_INPUT resize=WxH, then quit;
  game      the game booted from --disc in a --window, resized RESIZES times by an X11 client on the display
            (this script's --resizer mode, XResizeWindow through libX11, configured by SWAPCHAIN_RESIZER), ended by --frame-limit.

Pass criteria, read from the device's census line (rhi_destroy: "swapchain census: R recreates, F create
failures, S surface rebuilds, P presents, A after the last recreate"):
  - the fault knob logged its arming line (else VACUOUS: the path under test never ran);
  - R >= RESIZES (else VACUOUS when R == 0, FAIL otherwise);
  - no "vkCreateSwapchainKHR failed" line unless the next device line rebuilds the surface, and F == S: in
    mode window-held, F == 0 (the oldSwapchain handover releases the window); in window-held-surface, each
    rebuild refuses one creation, the surface rebuild releases the window and the retry succeeds;
  - A >= MIN_PRESENTS_AFTER: presents continue after the last rebuild.

usage: check_swapchain_rebuild.py {launcher|game} [--fault MODE] [--build-dir DIR] [--out DIR] [--game G]
Exit 0 PASS, 1 FAIL, 3 VACUOUS, 64 SKIP (binary, disc, xvfb-run, libX11 or the window manager missing).
"""
import argparse
import ctypes
import ctypes.util
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_launcher as cl  # noqa: E402

RESIZES = ["1000x700", "1200x760", "900x600", "1100x720"]
MIN_PRESENTS_AFTER = 30
# The launcher's script: a resize every 2.5 s from the first launcher frame, quit 4 s after the last.
LAUNCHER_SCRIPT = ",".join(f"{2500 * (i + 1)}:resize={s}" for i, s in enumerate(RESIZES)) + \
    f",{2500 * len(RESIZES) + 4000}:quit"
# The game's resizer: first resize this long after the window maps, then one every RESIZE_GAP_S.
GAME_FIRST_S = 8.0
RESIZE_GAP_S = 3.0
GAME_FRAME_LIMIT = 2400   # VBLs; the run ends on it

CENSUS = re.compile(r"swapchain census: (\d+) recreates, (\d+) create failures, (\d+) surface rebuilds, "
                    r"(\d+) presents, (\d+) after the last recreate")
ARMED = re.compile(r"fault injection: the window stays bound after swapchain destruction")
CREATE_FAIL = re.compile(r"vkCreateSwapchainKHR failed: (-?\d+)")
REBUILD = "rebuilding the surface"


def resizer(first_s, gap_s, sizes):
    """Run on $DISPLAY: wait for the largest viewable top-level window, then resize it through `sizes`."""
    lib = ctypes.util.find_library("X11")
    if not lib:
        print("[resizer] libX11 not found", file=sys.stderr)
        return 64
    x = ctypes.CDLL(lib)
    x.XOpenDisplay.restype = ctypes.c_void_p
    x.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x.XDefaultRootWindow.restype = ctypes.c_ulong
    x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
                             ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
                             ctypes.POINTER(ctypes.c_uint)]
    x.XResizeWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_uint, ctypes.c_uint]
    x.XFlush.argtypes = [ctypes.c_void_p]
    x.XFree.argtypes = [ctypes.c_void_p]

    class Attrs(ctypes.Structure):   # XWindowAttributes, Xlib.h order
        _fields_ = [("x", ctypes.c_int), ("y", ctypes.c_int), ("width", ctypes.c_int),
                    ("height", ctypes.c_int), ("border_width", ctypes.c_int), ("depth", ctypes.c_int),
                    ("visual", ctypes.c_void_p), ("root", ctypes.c_ulong), ("c_class", ctypes.c_int),
                    ("bit_gravity", ctypes.c_int), ("win_gravity", ctypes.c_int),
                    ("backing_store", ctypes.c_int), ("backing_planes", ctypes.c_ulong),
                    ("backing_pixel", ctypes.c_ulong), ("save_under", ctypes.c_int),
                    ("colormap", ctypes.c_ulong), ("map_installed", ctypes.c_int),
                    ("map_state", ctypes.c_int), ("all_event_masks", ctypes.c_long),
                    ("your_event_mask", ctypes.c_long), ("do_not_propagate_mask", ctypes.c_long),
                    ("override_redirect", ctypes.c_int), ("screen", ctypes.c_void_p)]
    x.XGetWindowAttributes.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(Attrs)]
    is_viewable = 2

    d = x.XOpenDisplay(None)
    if not d:
        print("[resizer] no display", file=sys.stderr)
        return 1

    def largest():
        root, parent = ctypes.c_ulong(), ctypes.c_ulong()
        kids, n = ctypes.POINTER(ctypes.c_ulong)(), ctypes.c_uint()
        if not x.XQueryTree(d, x.XDefaultRootWindow(d), ctypes.byref(root), ctypes.byref(parent),
                            ctypes.byref(kids), ctypes.byref(n)):
            return 0
        best, area = 0, 0
        for i in range(n.value):
            a = Attrs()
            if x.XGetWindowAttributes(d, kids[i], ctypes.byref(a)) and a.map_state == is_viewable \
                    and a.width * a.height > area:
                best, area = kids[i], a.width * a.height
        if kids:
            x.XFree(kids)
        return best

    w = 0
    for _ in range(1200):
        w = largest()
        if w:
            break
        time.sleep(0.1)
    if not w:
        print("[resizer] no window mapped", file=sys.stderr)
        return 2
    time.sleep(first_s)
    for s in sizes:
        ww, hh = map(int, s.split("x"))
        x.XResizeWindow(d, w, ww, hh)
        x.XFlush(d)
        print(f"[resizer] resize {s}", file=sys.stderr, flush=True)
        time.sleep(gap_s)
    return 0


def run_game(binary, d, game, fault):
    """The game from --disc in a 640x448 window, resized by this script's resizer on the same display."""
    disc = os.path.join(cl.ROOT, "games", game, game, "build", cl.DISCS[game] + ".cue")
    if not os.path.isfile(disc):
        raise cl.Skip(f"{disc} missing")
    if not cl.shutil.which("xvfb-run"):
        raise cl.Skip("xvfb-run not found")
    wm = cl.WM or os.path.join(os.path.dirname(binary), "openpete_xvfb_wm")
    if not os.access(wm, os.X_OK):
        raise cl.Skip(f"{wm} missing")
    e = {k: v for k, v in os.environ.items() if k != "WAYLAND_DISPLAY" and not k.startswith("OPENPETE_")}
    e.update({"SDL_VIDEODRIVER": "x11", "OPENPETE_MSGBOX_SKIP": "1", "OPENPETE_VIBRATION": "0",
              "XDG_DATA_HOME": os.path.join(d, "data"), "XDG_STATE_HOME": os.path.join(d, "state"),
              "XDG_CONFIG_HOME": os.path.join(d, "config")})
    if fault:
        e["OPENPETE_VK_FAULT"] = fault
    ready = os.path.join(d, "game.wm-ready")
    e["SWAPCHAIN_RESIZER"] = f"{GAME_FIRST_S}:{RESIZE_GAP_S}:" + ",".join(RESIZES)
    sh = ('"$0" --panel 0 --ready "$1" & until [ -e "$1" ]; do sleep 0.05; done; '
          '"$2" "$3" --resizer & r=$!; shift 3; "$@"; rc=$?; wait $r; exit $rc')
    cmd = ["xvfb-run", "-a", "-s", f"-screen 0 {cl.SCREEN}x24", "sh", "-c", sh, wm, ready, sys.executable,
           os.path.abspath(__file__), binary, "--no-config", "--no-card", "--vibration", "0", "--input-lock",
           "--mute", "1", "--disc", disc, "--window", "640x448", "--frame-limit", str(GAME_FRAME_LIMIT)]
    log = os.path.join(d, "game.log")
    with open(log, "w") as f:
        rc = cl.run_session(cmd, e, f, 240)
    with open(log, errors="replace") as f:
        return rc, f.read()


def verdict(case, rc, log, fault, logpath):
    """Print the census and the verdict; return the exit code."""
    census = CENSUS.findall(log)
    fails = CREATE_FAIL.findall(log)
    rhi = [ln for ln in log.splitlines() if ln.startswith("[rhi/vk]")]
    unrecovered = [i for i, ln in enumerate(rhi) if CREATE_FAIL.search(ln)
                   and not (i + 1 < len(rhi) and REBUILD in rhi[i + 1])]
    print(f"[swapchain] {case}: rc {rc}; census {census}; create-failure lines {len(fails)} "
          f"{sorted(set(fails))}; fault armed {bool(ARMED.search(log))}; log {logpath}")
    if fault and not ARMED.search(log):
        print(f"[swapchain] {case}: VACUOUS (OPENPETE_VK_FAULT={fault} never armed)")
        return 3
    if not census:
        print(f"[swapchain] {case}: FAIL (no census line: the device was not destroyed, rc {rc})")
        return 1
    rec, cfail, surf, pres, after = map(int, census[-1])
    print(f"[swapchain] {case}: engagement: {rec} recreates, {surf} surface rebuilds, {pres} presents, "
          f"{after} after the last recreate")
    if rec == 0 and not fails:
        print(f"[swapchain] {case}: VACUOUS (no swapchain rebuild ran)")
        return 3
    bad = []
    if rec < len(RESIZES):
        bad.append(f"{rec} recreates for {len(RESIZES)} resizes")
    if unrecovered or cfail != surf or (fault == "window-held" and cfail):
        bad.append(f"{cfail} create failures in the census ({len(fails)} lines, {len(unrecovered)} not followed "
                   f"by a surface rebuild) against {surf} surface rebuilds")
    if after < MIN_PRESENTS_AFTER:
        bad.append(f"{after} presents after the last recreate, need {MIN_PRESENTS_AFTER}")
    for b in bad:
        print(f"[swapchain] FAIL {case}: {b}")
    print(f"[swapchain] {case}: {'FAIL' if bad else 'PASS'}")
    return 1 if bad else 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--resizer":
        first, gap, sizes = os.environ["SWAPCHAIN_RESIZER"].split(":")
        return resizer(float(first), float(gap), sizes.split(","))
    ap = argparse.ArgumentParser()
    ap.add_argument("case", choices=["launcher", "game"])
    ap.add_argument("--fault", default="window-held", help="OPENPETE_VK_FAULT value; '' runs without a fault")
    ap.add_argument("--build-dir", default=os.path.join(cl.ROOT, "build", "linux-release"))
    ap.add_argument("--out", default=None)
    ap.add_argument("--game", default="spyro-1", choices=sorted(cl.DISCS))
    a = ap.parse_args()
    binary = os.path.join(a.build_dir, "openpete")
    if not os.access(binary, os.X_OK):
        print(f"[swapchain] SKIP: {binary} missing")
        return 64
    cl.WM = os.path.join(a.build_dir, "openpete_xvfb_wm")
    out = a.out or os.path.join(a.build_dir, "swapchain-tests")
    d = cl.case_dir(out, f"{a.case}-{a.fault or 'nofault'}")
    try:
        if a.case == "launcher":
            env = {"OPENPETE_LAUNCHER_INPUT": LAUNCHER_SCRIPT}
            if a.fault:
                env["OPENPETE_VK_FAULT"] = a.fault
            rc, log = cl.launch(binary, d, "launcher", ["--launcher-window", "1280x800"], env, timeout=120)
            logpath = os.path.join(d, "launcher.log")
        else:
            rc, log = run_game(binary, d, a.game, a.fault)
            logpath = os.path.join(d, "game.log")
    except cl.Skip as e:
        print(f"[swapchain] SKIP: {e}")
        return 64
    return verdict(a.case, rc, log, a.fault, logpath)


if __name__ == "__main__":
    sys.exit(main())
