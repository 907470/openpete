#!/usr/bin/env python3
"""inject_input.py — M5 Tier B: inject real OS input at scheduled wall times.

Everything inside the process is downstream of the thing that most needs
measuring. The pulse source built into openpete (OPENPETE_INPUT_PULSE) fires
just upstream of the latch, so it validates the log's plumbing and the
analyzer's arithmetic and NOTHING about the way in: it cannot see the SDL pump,
event delivery, focus, or a main thread parked in a modal drag loop. Worse,
during exactly that stall it publishes nothing and its own schedule slides with
it — it would absolve the hazard the host thread split introduced.

This injects from a separate process, through the OS, into the game's window.
It is therefore the only producer permitted to state an end-to-end input
latency, and the only one that can drive a KNOWN arrival phase, which is what
the latch-window pivot (I2) needs.

Timestamps are taken immediately AFTER the injection call returns, on the same
wall clock the pace log's CLOCKSAMPLE records pair against
(GetSystemTimePreciseAsFileTime on Windows, CLOCK_REALTIME elsewhere). Stamping
the SCHEDULED time instead would fold this process's own scheduler jitter into
the game's account.

Usage (Windows; run alongside the game with the game window focused):
    py scripts/pacing/inject_input.py --count 400 --out inject.jsonl

Then:
    pace.py input <log>.pacelog --inject inject.jsonl
"""
import argparse
import ctypes
import json
import os
import sys
import time

# Left Ctrl. PsyCross maps it to L2 (PsyX_main.cpp), which reaches the pad
# buffer like any other bit but advances no menu, so a sweep does not rewrite
# the scene it is measuring.
DEFAULT_SCANCODE = 0x1D
DEFAULT_PAD_BIT = 0x0100


# ------------------------------------------------------------------ Windows

class _KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", ctypes.c_ushort), ("wScan", ctypes.c_ushort),
                ("dwFlags", ctypes.c_uint), ("time", ctypes.c_uint),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulonglong))]


class _INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("ki", _KEYBDINPUT), ("_pad", ctypes.c_byte * 32)]
    _anonymous_ = ("u",)
    _fields_ = [("type", ctypes.c_uint), ("u", _U)]


KEYEVENTF_SCANCODE = 0x0008
KEYEVENTF_KEYUP = 0x0002


class WindowsInjector:
    name = "SendInput"

    def __init__(self, scancode):
        self.u32 = ctypes.WinDLL("user32", use_last_error=True)
        self.k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.winmm = ctypes.WinDLL("winmm")
        self.scancode = scancode
        # 1 ms timer resolution: without it Sleep granularity is ~15.6 ms and
        # the schedule cannot place a press anywhere inside a 16.7 ms period.
        self.winmm.timeBeginPeriod(1)

    def close(self):
        self.winmm.timeEndPeriod(1)

    def wall_ns(self):
        # The same source SDL_GetCurrentTime uses on Windows. GetTickCount or
        # time.time() would add their own (coarse, non-monotonic) error to the
        # game's account.
        ft = ctypes.c_ulonglong()
        self.k32.GetSystemTimePreciseAsFileTime(ctypes.byref(ft))
        # FILETIME is 100 ns ticks since 1601-01-01; 11644473600 s to Unix.
        return (ft.value - 116444736000000000) * 100

    def _send(self, flags):
        inp = _INPUT()
        inp.type = 1  # INPUT_KEYBOARD
        inp.ki = _KEYBDINPUT(0, self.scancode, flags | KEYEVENTF_SCANCODE,
                             0, None)
        n = self.u32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(_INPUT))
        if n != 1:
            raise OSError(f"SendInput refused (error {ctypes.get_last_error()}) "
                          f"— a non-elevated injector cannot reach an elevated "
                          f"window (UIPI)")

    def press(self):
        self._send(0)

    def release(self):
        self._send(KEYEVENTF_KEYUP)

    def find_window(self, title_substr):
        found = []

        @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
        def cb(hwnd, _lp):
            buf = ctypes.create_unicode_buffer(512)
            self.u32.GetWindowTextW(hwnd, buf, 512)
            if title_substr in buf.value and self.u32.IsWindowVisible(hwnd):
                found.append(hwnd)
            return True

        self.u32.EnumWindows(cb, None)
        return found[0] if found else None

    def raise_window(self, hwnd):
        """Bring a window to the foreground from a background process.

        Windows refuses SetForegroundWindow to a process that does not own the
        foreground, and returns success either way. Attaching to the current
        foreground thread's input queue lifts the restriction for the duration
        of the call; the result is checked rather than assumed."""
        SW_RESTORE = 9
        self.u32.ShowWindow(hwnd, SW_RESTORE)
        fg = self.u32.GetForegroundWindow()
        cur = self.k32.GetCurrentThreadId()
        other = self.u32.GetWindowThreadProcessId(fg, None) if fg else 0
        if other and other != cur:
            self.u32.AttachThreadInput(cur, other, True)
            self.u32.SetForegroundWindow(hwnd)
            self.u32.AttachThreadInput(cur, other, False)
        else:
            self.u32.SetForegroundWindow(hwnd)
        return self.u32.GetForegroundWindow() == hwnd

    def foreground_title(self):
        hwnd = self.u32.GetForegroundWindow()
        if not hwnd:
            return ""
        buf = ctypes.create_unicode_buffer(512)
        self.u32.GetWindowTextW(hwnd, buf, 512)
        return buf.value


def make_injector(scancode):
    if sys.platform == "win32":
        return WindowsInjector(scancode)
    # Linux would need /dev/uinput, which is root-only here, and under WSLg the
    # compositor does not route synthetic evdev to the focused surface. Saying
    # so beats shipping a path that silently injects into nothing: a Tier B leg
    # that delivers zero events fails by producing FEWER edges, not wrong ones,
    # which is invisible unless the run is refused outright.
    raise SystemExit(
        f"inject_input: no external injector on {sys.platform}. Tier B needs a "
        f"real desktop session: Windows (SendInput) is supported; Linux needs "
        f"write access to /dev/uinput AND a compositor that routes synthetic "
        f"evdev to the focused surface (WSLg does not).")


def sleep_until(wall_ns_fn, target_ns):
    """Sleep to within ~1.5 ms, then spin. A press placed by Sleep alone lands
    with millisecond granularity, which cannot resolve a 16.7 ms period."""
    while True:
        rem = target_ns - wall_ns_fn()
        if rem <= 0:
            return
        if rem > 1_500_000:
            time.sleep((rem - 1_500_000) / 1e9)
        # else: spin


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--count", type=int, default=400,
                    help="pulses to inject (>=1024 for a full phase sweep)")
    ap.add_argument("--gap-us", type=int, default=117_000,
                    help="interval between presses; deliberately NOT a whole "
                         "number of VBL periods, or every press lands at the "
                         "same phase and the sweep reports one bin")
    ap.add_argument("--hold-us", type=int, default=20_000,
                    help="how long each press is held; >= one VBL period "
                         "(16715 us) so a miss is a finding, not the expected "
                         "outcome of a press shorter than the sampling interval")
    ap.add_argument("--scancode", type=lambda v: int(v, 0),
                    default=DEFAULT_SCANCODE)
    ap.add_argument("--pad-bit", type=lambda v: int(v, 0), default=DEFAULT_PAD_BIT)
    ap.add_argument("--start-delay-s", type=float, default=8.0,
                    help="let the game boot and settle before the first press")
    ap.add_argument("--expect-title", default="Spyro the Dragon",
                    help="substring the foreground window title must contain. "
                         "Checked before EVERY press and it gates the press: "
                         "SendInput goes to whatever window is foreground, so "
                         "an unchecked injector types into the user's browser "
                         "when focus drifts. Pass an empty string to disable, "
                         "which also disables the delivery guarantee.")
    ap.add_argument("--focus", action="store_true",
                    help="raise the game window before starting. Off by "
                         "default: taking the foreground is a visible action "
                         "on someone's desktop, not a measurement detail.")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    inj = make_injector(a.scancode)
    # Opened before the first press and written as we go: an exception part way
    # through (the desktop locking, UIPI, the game exiting) would otherwise
    # discard a run that had already delivered hundreds of usable pulses.
    out = open(a.out, "w")
    try:
        wall = inj.wall_ns
        t0 = wall() + int(a.start_delay_s * 1e9)
        # Phases are spread by a per-index hash rather than by a fixed step:
        # a fixed step commensurate with the period aliases onto a few phases,
        # and the whole point is uniform coverage of the period.
        if a.focus and a.expect_title:
            hwnd = inj.find_window(a.expect_title)
            if not hwnd:
                raise SystemExit(f"--focus: no visible window matching "
                                 f"{a.expect_title!r}; is the game running?")
            if not inj.raise_window(hwnd):
                raise SystemExit("--focus: could not take the foreground. "
                                 "Refusing to inject, because the presses "
                                 "would land in whatever window has it.")

        recs, skipped = [], 0
        for k in range(a.count):
            jitter = (k * 2654435761) % 16_715_111
            target = t0 + k * a.gap_us * 1000 + jitter
            sleep_until(wall, target)

            # The check GATES the press. SendInput lands in the foreground
            # window whatever that is, so an injector that checks focus but
            # presses anyway is typing into someone else's application — and
            # the presses that did arrive are then not a sample of the
            # schedule, so the run is unusable in both directions at once.
            title = inj.foreground_title() if a.expect_title else ""
            if a.expect_title and a.expect_title not in title:
                skipped += 1
                recs.append({"seq": k, "scheduled_wall_ns": target,
                             "skipped": True, "foreground": title})
                out.write(json.dumps(recs[-1]) + "\n")
                continue

            inj.press()
            down = wall()
            sleep_until(wall, down + a.hold_us * 1000)
            inj.release()
            up = wall()
            recs.append({"seq": k, "down_wall_ns": down, "up_wall_ns": up,
                         "scheduled_wall_ns": target, "focused": True})
            out.write(json.dumps(recs[-1]) + "\n")
            out.flush()
            if (k + 1) % 50 == 0:
                print(f"  {k+1}/{a.count} scheduled, {len(recs)-skipped} "
                      f"injected", file=sys.stderr)
    finally:
        inj.close()
        out.close()

    thieves = {}
    for r in recs:
        if r.get("skipped"):
            thieves[r["foreground"]] = thieves.get(r["foreground"], 0) + 1

    meta = {"kind": "inject-meta", "injector": inj.name,
            "foreground_when_skipped": thieves,
            "platform": sys.platform, "scancode": a.scancode,
            "pad_bit": a.pad_bit, "hold_us": a.hold_us, "gap_us": a.gap_us,
            "count": a.count, "injected": len(recs) - skipped,
            "skipped_unfocused": skipped}
    # Rewritten with the metadata first, now that the totals are known. The
    # incremental file above is the crash copy; this is the analysable one, and
    # the metadata leads because it decides whether the run may be analysed at
    # all.
    with open(a.out, "w") as f:
        f.write(json.dumps(meta) + "\n")
        for r in recs:
            f.write(json.dumps(r) + "\n")
    print(f"wrote {len(recs) - skipped} injected pulse(s) to {a.out}"
          + (f" — {skipped} SKIPPED because the game window did not have "
             f"focus: {thieves}" if skipped else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
