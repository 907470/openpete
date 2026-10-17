#!/usr/bin/env python3
"""fleet-stream-check.py - the golden fleet coordinator's stream handling on this machine.

Run by tests/harness/check_fleet_stream.sh. scripts/golden-fleet.py is
imported; its Coordinator places jobs on a local host whose supervisor is a
fake (ship/supervise.sh sourcing a per-job, per-attempt plan written here),
so the real POLL_SCRIPT, KILL_SCRIPT, LAUNCH_SCRIPT, run_jobs, poll, stop
and unit-end check run against stream files and verdict reports the plans
write. Cases (each prints its counts):

  framing     POLL_SCRIPT frames state before bytes, an incomplete last line
              waits for the next poll, offsets continue across polls
  pool        pb.digest_files' process pool after the heat-diff import
  equality    a full unit's stream equals its report (SAME + DIVERGE =
              compared; DIVERGE, EXTRA, EXCEED, CREDITS sets)
  drop-div    OPENPETE_STREAM_DROP of a DIVERGE line: stream mismatch, rc 1
  drop-same   OPENPETE_STREAM_DROP of a SAME line on a passing unit: rc 0 -> 1
  extra       a unit whose only moves are EXTRA tiles (new rows): the stream
              pulls each tile without a heat diff, the accepted pull keeps
              it in the stream directory, promote_select sees it
  absent      a streamed and reported tile absent from the capture: the
              stream directory lacks it, stream mismatch names it
  exempt      an attempt without END (a timed-out leg) and a SKIP unit
  kill        lines written in the last poll interval are read before the
              kill; the re-queued attempt's lines equal the uninterrupted
              run's and pass the unit-end check
  stop        running, pending, local (golden lock held until teardown),
              racing completion both ways; no ledger row for a stopped unit
  stop-sync   a stop during a sync stand-in (a host script with a sleeping
              child) ends the run within STOP_POLL_S + 2 s: both processes
              gone, every unit stopped, done reason=stopped by user; a
              request naming another unit does not cover the run
  reap        REAP_SCRIPT ends a process carrying the run tag and spares an
              untagged one; a remote command and rsync's server carry the tag
  clean       build/fleet-stream/<run-id> removed once its run directory is gone
  expect      EXPECTED 1/0/0, UNEXPECTED 0/1/0, EXPECTED-NOT-MOVED 0/0/1,
              NOT-REACHED on a stopped unit and a timed-out row, typo and
              unknown-unit refusals (function and `run --expect`), resumed
              counts equal uninterrupted ones, summary counts equal the
              tagged lines of stream.log
  canary-verdict  canary-verdict.txt over identical canaries (both hosts
              pass), a differing tile (both fail), a canary leg that did not
              capture (it fails, the lone captured host fails with no
              cross-host comparison) and one host (canary=off); a pulled
              capture record stamped with fleet-run, fleet-host and
              fleet-run-dir

  move        `move` requests: an unknown unit, an unadmitted host and the
              unit's own host refused; an accepted move to a second host of
              the run carries the stamped rows (not an unstamped tile), the
              re-queued attempt runs pinned there outside the attempt budget,
              its stream covers every row, no ledger row; a finished unit
              refused; the CLI's request line, unknown host, done run and
              unknown run
  keep        a stopped unit's stamped rows and ship kept under kept/ at
              teardown, read by resolve_saved; unit_results reads the
              logged results `resume` decides on
  keep-exhausted  a unit past its deadline on every attempt ends `attempts
              exhausted` and its last attempt's stamped rows are kept the
              same way
  ledger      a whole unit writes a ledger row; a filtered run's unit and an
              accepted attempt that imported stamped rows write none, so the
              deadline on a speed-0.25 host stays the whole unit's time /
              speed x 2

Exit: 0 every case holds, 1 a case failed (named).
"""
import argparse
import fcntl
import importlib.util
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.abspath(os.path.join(HERE, "..", ".."))
_spec = importlib.util.spec_from_file_location("golden_fleet", os.path.join(TREE, "scripts", "golden-fleet.py"))
gf = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gf)
gf.POLL_S = 0.3

FAILS = []
GAME, TIER = "spyro-2", "presentation"
UNIT = f"{GAME}-{TIER}"
TILES = sorted(r["tile"] for r in gf.load_pb(TREE).load_manifest(GAME, TIER).rows)


def check(name, ok, detail):
    """Print a case's verdict and counts; record a failure."""
    print(("ok   " if ok else "FAIL ") + f"{name}: {detail}")
    if not ok:
        FAILS.append(name)


FAKE_SUP = r'''#!/usr/bin/env bash
set -u
source "$1"
''' + gf.SH_STARTTIME + r'''
mkdir -p "$UNIT_DIR/cand"
echo "$$ $(starttime $$)" >"$ATTEMPT_BASE.pid"
emit() { printf '%s %s\n' "$*" "$(date +%s.%3N)" >>"$ATTEMPT_BASE.stream"; }
emit_imported() { printf '%s %s imported\n' "$*" "$(date +%s.%3N)" >>"$ATTEMPT_BASE.stream"; }
report() { printf '%s\n' "$@" >"$UNIT_DIR/cand/verdict-report.txt"; }
finish() {
    printf 'rc=%s\nstatus=%s\nattempt=%s\nwall=%s\ncand-bytes=0\nmin-memavail-kib=0\n' "$1" "$2" "$ATTEMPT" "${WALL:-1}" \
        >"$ATTEMPT_BASE.done.tmp"
    mv -f "$ATTEMPT_BASE.done.tmp" "$ATTEMPT_BASE.done"
    exit 0
}
plan="$RUN_DIR/ship/plan-$JOB-$ATTEMPT.sh"
[ -f "$plan" ] || plan="$RUN_DIR/ship/plan-$JOB.sh"
source "$plan"
finish 1 plan-fell-through
'''


class FakeCo(gf.Coordinator):
    """A Coordinator over one local host whose supervisor is FAKE_SUP; results land under a temp directory."""

    def __init__(self, tmp, name):
        """Build the run directories, the host and the ledger redirect."""
        self.tmp = os.path.join(tmp, name)
        run_id = f"test-{name}"
        local = os.path.join(self.tmp, "local")
        os.makedirs(local)
        a = argparse.Namespace(game=GAME, tier=TIER, all=False, all_tiers=False, hosts="",
                               no_local=False, pull="divergent", expect=None)
        super().__init__(a, run_id, local)
        root = os.path.join(self.tmp, "root")
        host = gf.Host("local", {"transport": "local", "fleet_root": root})
        hs = gf.HostState(host, {"root": root, "engine": "podman"}, 1, run_id)
        os.makedirs(os.path.join(hs.run_dir, "ship"))
        with open(os.path.join(hs.run_dir, "ship", "supervise.sh"), "w") as f:
            f.write(FAKE_SUP)
        from PIL import Image
        Image.new("RGB", (4, 4), (7, 0, 0)).save(os.path.join(hs.run_dir, "ship", "tile.png"))
        self.hosts = [hs]
        self.hs = hs
        self.skew = {"local": 0.0}
        self.skip_polls = 0
        self.kill_hook = None

    def plan(self, job, body, attempt=None):
        """Write a job's plan (bash sourced by FAKE_SUP), for one attempt or every attempt."""
        suffix = f"-{attempt}" if attempt else ""
        with open(os.path.join(self.hs.run_dir, "ship", f"plan-{job}{suffix}.sh"), "w") as f:
            f.write(body)

    def pull_target(self, game, tier):
        """Pull into the case's own directory."""
        return os.path.join(self.tmp, "pull")

    def stream_dir(self, job):
        """Stream pulls into the case's own directory."""
        return os.path.join(self.tmp, "stream", job.name)

    def pull_unit(self, job, hs):
        """Pull the fake report and its tiles, keep the run candidate; no manifest-hash refusal."""
        local = self.pull_target(job.game, job.tier)
        shutil.rmtree(local, ignore_errors=True)
        rdir = f"{job.result['unit_dir']}/cand"
        rc, err = gf.rsync_pull(hs.host, rdir, local, names=["verdict-report.txt"])
        if rc != 0:
            return [err]
        report = gf.parse_report(os.path.join(local, "verdict-report.txt"))
        self.note_census(job, report)
        self.pull_tiles(hs, rdir, local, report)
        self.keep_run_candidate(job, local)
        return []

    def poll(self, hs, pending, on_done):
        """Skip the first skip_polls polls (the host looks unreachable), then poll."""
        if self.skip_polls:
            self.skip_polls -= 1
            return
        super().poll(hs, pending, on_done)

    def kill_attempt(self, att):
        """Run the case's hook (lines written in the last poll interval), then kill."""
        if self.kill_hook:
            self.kill_hook(att)
        super().kill_attempt(att)

    def go(self, jobs, deadline_s=60):
        """Run the jobs to their results; return the wall time."""
        self.jobs = jobs
        t0 = time.monotonic()
        self.run_jobs(jobs, deadline=lambda _j, _h: deadline_s, on_done=self.unit_done)
        return time.monotonic() - t0

    def stop_later(self, delay, line):
        """Write the stop request after delay seconds."""
        def write():
            with open(self.stop_path + ".tmp", "w") as f:
                f.write(line + "\n")
            os.replace(self.stop_path + ".tmp", self.stop_path)
        threading.Timer(delay, write).start()

    def stream_log_lines(self):
        """The run's stream.log lines (comment lines excluded)."""
        if not os.path.exists(self.stream_log):
            return []
        return [ln for ln in open(self.stream_log).read().splitlines() if not ln.startswith("#")]


def full_unit(div, same, extra, ident, exceed, credits_rc=0, absent=()):
    """A plan streaming a full unit and writing the matching report and done marker (rc 1 when anything moved).

    Each moved tile (both members of an EXCEED pair) is written into the
    candidate directory before the stream lines, except the tiles in absent.
    """
    moved = bool(div or extra or exceed or credits_rc not in (0, 64))
    rc = 1 if moved else 0
    files = list(div) + list(extra) + [n for t in exceed for n in (t, t.replace("__ident__", "__canon__"))]
    lines = [f'cp "$RUN_DIR/ship/tile.png" "$UNIT_DIR/cand/{t}"' for t in files if t not in absent]
    lines += [f"emit SAME {t} 0123456789abcdef" for t in same]
    lines += [f"emit DIVERGE {t} '0123456789abcdef->fedcba9876543210'" for t in div]
    lines += [f"emit EXTRA {t} '->fedcba9876543210'" for t in extra]
    lines += [f"emit IDENT {t} pixels=0/0" for t in ident]
    lines += [f"emit EXCEED {t} pixels=9/0" for t in exceed]
    if credits_rc != 64:
        lines.append(f"emit CREDITS rc={credits_rc}")
    lines.append("emit END")
    rep = [f"verdict: {'FAIL' if rc else 'PASS'}", f"exit: {rc}", f"compared: {len(same) + len(div)}",
           f"credits-rc: {credits_rc}"]
    rep += [f"divergent: {t} pixels=1 maxch=128 reference=store" for t in div]
    rep += [f"extra: {t}" for t in extra]
    rep += [f"identity-exceed: {t} {t.replace('__ident__', '__canon__')} pixels=9 baseline=0" for t in exceed]
    body = "\n".join(lines) + "\nreport " + " ".join(f"'{r}'" for r in rep) + f"\nfinish {rc} verdict\n"
    return body


def ledger_rows():
    """Rows of the redirected ledger."""
    p = gf.ledger_path()
    return open(p).read().splitlines()[1:] if os.path.exists(p) else []


def case_framing(tmp):
    """POLL_SCRIPT framing over two polls with an incomplete line between them."""
    d = os.path.join(tmp, "framing")
    os.makedirs(d)
    base = os.path.join(d, "attempt-1")
    host = gf.Host("local", {"transport": "local"})
    with open(base + ".stream", "w") as f:
        f.write("SAME a-vk.png 0123 1.000\nDIVERGE b-vk.png 01->23 2.0")
    with open(base + ".done", "w") as f:
        f.write("rc=1\nstatus=verdict\nattempt=1\n")
    out = host.run_script(gf.POLL_SCRIPT, [base, 0], binary=True).stdout
    fr = gf.parse_poll(out)
    state, data = fr[base]
    ok = out.startswith(b"state ") and state.startswith("done rc=1") and data.endswith(b"2.0")
    co = FakeCo(tmp, "framing-co")
    job = gf.Job("unit", GAME, TIER)
    att = gf.Attempt(job, co.hs, 60)
    co.take_stream(att, data)
    n1 = len(att.lines)
    with open(base + ".stream", "a") as f:
        f.write("00\nEND 3.000\n")
    fr2 = gf.parse_poll(host.run_script(gf.POLL_SCRIPT, [base, att.stream_off], binary=True).stdout)
    co.take_stream(att, fr2[base][1])
    verbs = [r["verb"] for r in att.lines]
    ok = ok and n1 == 1 and verbs == ["SAME", "DIVERGE", "END"] and att.lines[1]["epoch"] == 2.0 \
        and att.stream_off == os.path.getsize(base + ".stream")
    try:
        gf.parse_poll(b"state x running\nstream x 9\nabc")
        torn = False
    except ValueError:
        torn = True
    check("framing", ok and torn, f"state before bytes; {n1} line after poll 1, {len(verbs)} after "
          f"poll 2 ({' '.join(verbs)}); offset {att.stream_off}; torn frame refused: {torn}")


def case_pool(tmp):
    """The coordinator's pb.digest_files process pool still works after the heat-diff module is imported."""
    from PIL import Image
    d = os.path.join(tmp, "pool")
    os.makedirs(d)
    paths = []
    for i in range(40):
        paths.append(os.path.join(d, f"t{i}-vk.png"))
        Image.new("RGB", (8, 8), (i, 0, 0)).save(paths[-1])
    co = FakeCo(tmp, "pool-co")
    co.load_diffmod()
    digs = co.pb.digest_files(paths)
    check("pool-coordinator", sum(1 for v in digs.values() if v) == 40,
          f"{len(digs)} tiles digested through the pool after the diff module's import")


def case_stream_off(tmp):
    """OPENPETE_FLEET_STREAM=0: units get STREAM=0 and no unit-end check."""
    os.environ["OPENPETE_FLEET_STREAM"] = "0"
    co = FakeCo(tmp, "stream-off")
    del os.environ["OPENPETE_FLEET_STREAM"]
    on = FakeCo(tmp, "stream-on")
    job = gf.Job("unit", GAME, TIER)
    env_off = [kv for kv in co.job_env(job, co.hs, gf.Attempt(job, co.hs, 60)) if kv.startswith("STREAM=")]
    env_on = [kv for kv in on.job_env(job, on.hs, gf.Attempt(job, on.hs, 60)) if kv.startswith("STREAM=")]
    canary = gf.Job("canary", GAME, TIER, pinned="local")
    env_can = [kv for kv in on.job_env(canary, on.hs, gf.Attempt(canary, on.hs, 60)) if kv.startswith("STREAM=")]
    check("stream-off", env_off == ["STREAM=0"] and env_on == ["STREAM=1"] and env_can == ["STREAM=0"],
          f"OPENPETE_FLEET_STREAM=0 unit {env_off}, default unit {env_on}, canary {env_can}")


def case_equality(tmp):
    """A full unit's stream equals its report; drops of a DIVERGE and a SAME line are refused."""
    ident = [t.replace("__canon__", "__ident__") for t in TILES[:2] if "__canon__" in t]
    co = FakeCo(tmp, "equal")
    co.plan(UNIT, full_unit(div=TILES[:1], same=TILES[1:6], extra=["zz__canon__1-vk.png"],
                            ident=ident[:1], exceed=ident[1:2]))
    job = gf.Job("unit", GAME, TIER)
    co.go([job])
    r = job.result
    check("equality", r["rc"] == 1 and not r.get("stream_mismatch") and r.get("stream") != "exempt",
          f"{len(job.atts[1].lines)} lines; SAME 5 + DIVERGE 1 = compared 6; rc {r['rc']} "
          f"(the report's), mismatch: {r.get('stream_mismatch') or 'none'}")
    for name, drop, same, div in (("drop-div", TILES[0], TILES[1:6], TILES[:1]),
                                  ("drop-same", TILES[1], TILES[1:6], [])):
        os.environ["OPENPETE_STREAM_DROP"] = drop
        co = FakeCo(tmp, name)
        del os.environ["OPENPETE_STREAM_DROP"]
        co.plan(UNIT, full_unit(div=div, same=same, extra=[], ident=[], exceed=[]))
        job = gf.Job("unit", GAME, TIER)
        co.go([job])
        r = job.result
        want_rc0 = 1 if div else 0
        check(name, r["rc"] == 1 and "stream mismatch" not in r["status"] and bool(r.get("stream_mismatch")),
              f"dropped {drop}; report rc {want_rc0} -> unit rc {r['rc']}; "
              f"mismatch: {r.get('stream_mismatch')}")


def case_extra(tmp):
    """EXTRA tiles are streamed, pulled without a heat diff and kept promotable; an absent moved tile is a stream mismatch."""
    import contextlib
    import io
    pb = gf.load_pb(TREE)
    new = ["zz-new__canon__1-vk.png", "zz-new__canon__2-vk.png"]
    co = FakeCo(tmp, "extra")
    co.plan(UNIT, full_unit(div=[], same=TILES[:4], extra=new, ident=[], exceed=[]))
    job = gf.Job("unit", GAME, TIER)
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        co.go([job])
    log = out.getvalue()
    sys.stdout.write(log)
    dest = co.stream_dir(job)
    pulled = [t for t in new if f"stream pulled {t}: new row 4x4, no reference, no heat diff" in log]
    kept = [t for t in new if os.path.isfile(os.path.join(dest, t))]
    heat = os.listdir(os.path.join(dest, "diff")) if os.path.isdir(os.path.join(dest, "diff")) else []
    sel = pb.promote_select(dest, True, [])
    r = job.result
    check("extra", r["rc"] == 1 and not r.get("stream_mismatch") and pulled == new and kept == new
          and not heat and sel == new,
          f"rc {r['rc']} (the report's), mismatch: {r.get('stream_mismatch') or 'none'}; streamed "
          f"pulls {len(pulled)} of {len(new)}, kept {len(kept)}, heat diffs {len(heat)}, "
          f"promote_select {sel}")

    co = FakeCo(tmp, "absent")
    co.plan(UNIT, full_unit(div=TILES[:1], same=TILES[1:4], extra=new, ident=[], exceed=[],
                            absent=new[1:]))
    job = gf.Job("unit", GAME, TIER)
    co.go([job])
    mm = job.result.get("stream_mismatch") or ""
    check("absent", job.result["rc"] == 1 and "EXTRA 1 tile(s) absent" in mm and new[1] in mm
          and "DIVERGE" not in mm and "stream mismatch" not in job.result["status"],
          f"rc {job.result['rc']}; mismatch: {mm or 'none'}")


def case_exempt(tmp):
    """An attempt without END and a SKIP unit are exempt from the unit-end check."""
    co = FakeCo(tmp, "exempt")
    co.plan(UNIT, f"emit SAME {TILES[0]} 0123456789abcdef\n"
                  "report 'verdict: FAIL' 'exit: 1' 'compared: 0'\nfinish 1 verdict\n")
    job = gf.Job("unit", GAME, TIER)
    co.go([job])
    co2 = FakeCo(tmp, "exempt-skip")
    co2.plan(UNIT, "finish 64 capture-skip\n")
    job2 = gf.Job("unit", GAME, TIER)
    co2.go([job2])
    check("exempt", job.result.get("stream") == "exempt" and not job.result.get("stream_mismatch")
          and job2.result.get("stream") == "exempt" and job2.result["rc"] == 64,
          f"no END: rc {job.result['rc']}, {job.result.get('stream')}; SKIP: rc {job2.result['rc']}, "
          f"{job2.result.get('stream')}")


def case_kill(tmp):
    """Lines written in the last poll interval are read before KILL_SCRIPT; the re-queued attempt passes the check."""
    a, b, c = TILES[0:2], TILES[2:4], TILES[4:6]
    co = FakeCo(tmp, "kill")
    go_file, written = os.path.join(co.tmp, "go"), os.path.join(co.tmp, "written")
    rows_ab = "\n".join(f"emit SAME {t} 0123456789abcdef" for t in a)
    rows_b = "\n".join(f"emit SAME {t} 0123456789abcdef" for t in b)
    co.plan(UNIT, f"{rows_ab}\nwhile [ ! -e {go_file} ]; do sleep 0.05; done\n{rows_b}\n"
                  f"touch {written}\nsleep 60\n", attempt=1)
    imported = "\n".join(f"emit_imported SAME {t} 0123456789abcdef" for t in a + b)
    co.plan(UNIT, imported + "\n" + "\n".join(f"emit SAME {t} 0123456789abcdef" for t in c)
            + "\nemit CREDITS rc=0\nemit END\n"
            + f"report 'verdict: PASS' 'exit: 0' 'compared: 6' 'credits-rc: 0'\nfinish 0 verdict\n",
            attempt=2)

    def hook(att):
        if att.n == 1:
            open(go_file, "w").close()
            t0 = time.monotonic()
            while not os.path.exists(written) and time.monotonic() - t0 < 10:
                time.sleep(0.05)
    co.kill_hook = hook
    co.first_deadline = 1
    job = gf.Job("unit", GAME, TIER)
    co.go([job], deadline_s=60)
    late = [ln for ln in co.stream_log_lines() if ln.split()[2] == "1" and ln.split()[5] in b]
    ref = FakeCo(tmp, "kill-ref")
    ref.plan(UNIT, "\n".join(f"emit SAME {t} 0123456789abcdef" for t in a + b + c)
             + "\nemit CREDITS rc=0\nemit END\n"
             + "report 'verdict: PASS' 'exit: 0' 'compared: 6' 'credits-rc: 0'\nfinish 0 verdict\n")
    rjob = gf.Job("unit", GAME, TIER)
    ref.go([rjob])
    got = {gf.stream_key(x) for x in job.atts[2].lines}
    want = {gf.stream_key(x) for x in rjob.atts[1].lines}
    check("kill", len(late) == 2 and job.result["rc"] == 0 and job.result.get("attempt") == 2
          and got == want and not job.result.get("stream_mismatch"),
          f"{len(late)} of 2 lines written in the last interval read before the kill; attempt 2 "
          f"{len(got)} keys == uninterrupted {len(want)}: {got == want}; rc {job.result['rc']}")


def case_stop(tmp):
    """Stop of a running, a pending and a local unit, and a stop racing completion (both orders)."""
    n0 = len(ledger_rows())
    co = FakeCo(tmp, "stop-running")
    co.golden_fd = gf.try_golden_lock(co.hs.root)
    co.plan(UNIT, f"emit SAME {TILES[0]} 0123456789abcdef\nsleep 60\n")
    job = gf.Job("unit", GAME, TIER)
    co.stop_later(1.0, f"unit={UNIT}")
    wall = co.go([job])
    pid = open(job.atts[1].base + ".pid").read().split()[0]
    alive = os.path.exists(f"/proc/{pid}")
    marker = os.path.exists(os.path.join(co.stream_dir(job), "STOPPED"))
    lock = os.path.join(gf.expand_root(co.hs.root), "golden.lock")
    held = subprocess.run(["flock", "-n", lock, "true"]).returncode != 0
    co.teardown()
    free = subprocess.run(["flock", "-n", lock, "true"]).returncode == 0
    check("stop-running+local", job.result["status"] == gf.STOP_STATUS and job.result["rc"] == 1
          and job.stopped and not alive and marker and wall < 10 and len(ledger_rows()) == n0 and held and free,
          f"stopped after {wall:.1f} s, supervisor alive: {alive}, STOPPED marker: {marker}, "
          f"golden lock held before teardown: {held}, free after: {free}, ledger rows +{len(ledger_rows()) - n0}")

    co = FakeCo(tmp, "stop-pending")
    co.plan("first", "sleep 1\nreport 'verdict: PASS' 'exit: 0'\nfinish 0 verdict\n")
    first = gf.Job("unit", GAME, TIER, label="first")
    second = gf.Job("unit", GAME, TIER, label="second")
    co.stop_later(0.0, "unit=second")
    time.sleep(0.1)
    co.go([first, second])
    launched = os.path.exists(os.path.join(co.hs.run_dir, "u-second"))
    check("stop-pending", second.result["status"] == gf.STOP_STATUS and second.result["host"] == "-"
          and not launched and first.result["rc"] == 0 and len(ledger_rows()) == n0 + 1,
          f"second: {second.result['status']} on {second.result['host']}, launched: {launched}; "
          f"first rc {first.result['rc']}; ledger rows +{len(ledger_rows()) - n0} (first only)")

    co = FakeCo(tmp, "stop-race-done-first")
    co.plan(UNIT, "report 'verdict: PASS' 'exit: 0'\nfinish 0 verdict\n")
    job = gf.Job("unit", GAME, TIER)
    co.stop_later(0.15, f"unit={UNIT}")
    co.go([job])
    check("stop-race-done-read-first", job.result["rc"] == 0 and not job.stopped
          and len(ledger_rows()) == n0 + 2,
          f"done read before the request: rc {job.result['rc']} ({job.result['status']}), "
          f"ledger rows +{len(ledger_rows()) - n0} (its own row kept)")

    co = FakeCo(tmp, "stop-race-unread")
    co.skip_polls = 1
    co.plan(UNIT, "report 'verdict: PASS' 'exit: 0'\nfinish 0 verdict\n")
    job = gf.Job("unit", GAME, TIER)
    co.stop_later(0.1, f"unit={UNIT}")
    co.go([job])
    done = os.path.exists(job.atts[1].base + ".done")
    check("stop-race-done-unread", job.stopped and job.result["status"] == gf.STOP_STATUS and done
          and len(ledger_rows()) == n0 + 2,
          f"done marker on the host unread at the request: {done}; result {job.result['status']}; "
          f"ledger rows +{len(ledger_rows()) - n0} (no row for it)")
    co.jobs = [job]
    rc = co.run_rc()
    check("stop-run-verdict", rc == 1 and co.done_reason == gf.STOP_STATUS,
          f"run rc {rc}, reason={co.done_reason}")


def case_stop_cli(tmp):
    """`golden-fleet.py stop`: request lines, accumulation, refusal once done, unknown run."""
    root = os.path.join(tmp, "cli-root")
    run_dir = os.path.join(root, "runs", "r1")
    os.makedirs(run_dir)
    gf.local_fleet_root = lambda: root
    rc1 = gf.main(["stop", "r1", "--unit", UNIT])
    rc2 = gf.main(["stop", "r1"])
    text = open(os.path.join(run_dir, "stop-request")).read()
    open(os.path.join(run_dir, "done"), "w").write("rc=0\n")
    rc3 = gf.main(["stop", "r1"])
    rc4 = gf.main(["stop", "no-such-run"])
    check("stop-cli", (rc1, rc2, rc3, rc4) == (0, 0, 1, 2) and text == f"all\nunit={UNIT}\n"
          and gf.read_stop_request(os.path.join(run_dir, "stop-request")) == {"all", UNIT},
          f"exits {rc1}, {rc2}, {rc3} (done), {rc4} (no run); request {text.split()}")


class SyncCo(FakeCo):
    """A FakeCo whose preflight is skipped and whose sync is a host script sleeping in a child, its pids recorded."""

    def prepare_hosts(self, src_bytes):
        """Select the local host without preflight, clean or image ship."""
        self.sel = [self.hs.host]

    def build_and_sync(self):
        """Run a 60 s sync stand-in (a script and its sleeping child) on the host."""
        self.mkdirs(self.hs)
        self.script(self.hs, 'echo $$ >"$1/sync.pid"; sleep 60 & echo $! >"$1/child.pid"; wait',
                    [self.tmp], timeout=300)
        raise AssertionError("the sync stand-in returned")


def case_stop_sync(tmp):
    """Stop during the sync: the sync's process group dies, every unit is stopped, the done marker says so."""
    co = SyncCo(tmp, "stop-sync")
    co.stop_later(2.0, "all")
    t_req = time.monotonic() + 2.0
    prev = {s: signal.getsignal(s) for s in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)}
    try:
        rc, _co = gf.drive(co.run_id, co.local_run_dir, lambda: co)
    finally:
        for s, h in prev.items():
            signal.signal(s, h)
    lag = time.monotonic() - t_req
    pids = [open(os.path.join(co.tmp, n)).read().split()[0] for n in ("sync.pid", "child.pid")]
    alive = [p for p in pids if os.path.exists(f"/proc/{p}")]
    done = gf.parse_kv(open(os.path.join(co.local_run_dir, "done")).read())
    job = co.jobs[0] if co.jobs else None
    marker = job is not None and os.path.exists(os.path.join(co.stream_dir(job), "STOPPED"))
    bound = gf.STOP_POLL_S + 2.0
    check("stop-sync", rc == 1 and done.get("reason") == gf.STOP_STATUS and job is not None
          and job.stopped and job.result["host"] == "-" and marker and not alive and lag < bound,
          f"run ended {lag:.1f} s after the request (bound {bound:.1f} s), rc {rc}, done "
          f"reason={done.get('reason')}, units stopped {sum(j.stopped for j in co.jobs)}/"
          f"{len(co.jobs)}, STOPPED marker: {marker}, sync processes alive: {len(alive)} of 2")

    co = SyncCo(tmp, "stop-sync-scope")
    got = []
    for line in ("unit=other", f"unit={UNIT}", "all"):
        with open(co.stop_path, "w") as f:
            f.write(line + "\n")
        got.append(co.stop_covers_run())
    check("stop-sync-scope", got == [False, True, True],
          f"covers the run for unit=other, unit={UNIT}, all: {got}")


def case_reap(tmp):
    """REAP_SCRIPT ends a process carrying the run tag and spares an untagged one; remote argv and rsync carry the tag."""
    tag = f"test-reap-{os.getpid()}"
    tagged = subprocess.Popen(["sleep", "60"], env=dict(os.environ, **{gf.RUN_TAG_VAR: tag}))
    other = subprocess.Popen(["sleep", "60"])
    host = gf.Host("local", {"transport": "local"})
    out = host.run_script(gf.REAP_SCRIPT, [gf.RUN_TAG_VAR, tag], timeout=60, tag=False).stdout.strip()
    tagged_rc, other_alive = tagged.wait(5), other.poll() is None
    other.kill()
    other.wait()
    remote = gf.Host("r", {"transport": "ssh", "ssh": "u@h"})
    with gf.run_tag(tag):
        argv = remote.argv("bash -s --")
        rsync = gf.rsync_base(remote)
    plain = remote.argv("bash -s --")
    check("reap", tagged_rc is not None and other_alive and out == "reaped 1 process(es)"
          and argv[-1] == f"{gf.RUN_TAG_VAR}={tag} bash -s --" and plain[-1] == "bash -s --"
          and f"--rsync-path={gf.RUN_TAG_VAR}={tag} rsync" in rsync,
          f"reaper: {out!r}; tagged exit {tagged_rc}, untagged alive: {other_alive}; "
          f"remote command {argv[-1]!r}; rsync {rsync[-1]!r}")


def case_clean(tmp):
    """clean removes build/fleet-stream/<run-id> (STOPPED marker included) once the run directory is gone."""
    tree, root = os.path.join(tmp, "clean-tree"), os.path.join(tmp, "clean-root")
    for run_id in ("kept", "gone"):
        d = os.path.join(tree, gf.STREAM_DIR, run_id, UNIT)
        os.makedirs(d)
        open(os.path.join(d, "STOPPED"), "w").close()
    os.makedirs(os.path.join(root, "runs", "kept"))
    gf.clean_stream_dirs(tree, root)
    left = sorted(os.listdir(os.path.join(tree, gf.STREAM_DIR)))
    check("clean", left == ["kept"], f"left {left} (the run directory of `kept` exists)")


def rec(verb, tile, imported=False):
    """A stream record."""
    return {"verb": verb, "tile": tile, "detail": "x", "epoch": 1.0, "imported": imported}


def case_expect(tmp):
    """Expected-moves tags, statuses, counts and refusals."""
    poked, quiet = TILES[0], TILES[1]
    blessed = set(TILES)
    lines = [rec("DIVERGE", poked), rec("SAME", quiet), rec("END", "END")]
    e_poked = gf.Expectation(UNIT, poked, "row poke", 1)
    e_quiet = gf.Expectation(UNIT, quiet, "predicted, not moved", 2)
    c1, _ = gf.expect_counts([e_poked], {UNIT: (True, lines, blessed)})
    c2, _ = gf.expect_counts([], {UNIT: (True, lines, blessed)})
    c3, r3 = gf.expect_counts([e_quiet], {UNIT: (True, [rec("SAME", quiet)], blessed)})
    fmt = (lambda c: f"{c['EXPECTED']}/{c['UNEXPECTED']}/{c['EXPECTED-NOT-MOVED']}")
    check("expect-expected", fmt(c1) == "1/0/0", f"counts {fmt(c1)}")
    check("expect-unexpected", fmt(c2) == "0/1/0", f"counts {fmt(c2)}")
    check("expect-not-moved", fmt(c3) == "0/0/1" and r3[0][1] == "EXPECTED-NOT-MOVED",
          f"counts {fmt(c3)}, status {r3[0][1]}")
    _c, r4 = gf.expect_counts([e_quiet], {UNIT: (False, [], blessed)})
    row = TILES[0].split("__")[0]
    e_row = gf.Expectation(UNIT, row + "__", "timed-out row", 3)
    other = [rec("SAME", t) for t in TILES if not t.startswith(row + "__")][:20]
    _c, r5 = gf.expect_counts([e_row], {UNIT: (True, other, blessed)})
    check("expect-not-reached", r4[0][1] == "NOT-REACHED" and r5[0][1] == "NOT-REACHED",
          f"stopped unit: {r4[0][1]}; timed-out row {row} ({len(r5[0][2])} tiles, none streamed): {r5[0][1]}")
    pb = gf.load_pb(TREE)
    errs = gf.expect_refusals(TREE, pb, [(GAME, TIER)], [gf.Expectation(UNIT, "no-such-row-xyzzy", "typo", 1),
                                                        gf.Expectation("spyro-9-presentation", "a", "unit", 2)])
    path = os.path.join(tmp, "bad-expect.txt")
    with open(path, "w") as f:
        f.write(f"{UNIT} no-such-row-xyzzy typo\n")
    cli = subprocess.run([sys.executable, os.path.join(TREE, "scripts", "golden-fleet.py"), "run",
                          "--game", GAME, "--expect", path], capture_output=True, text=True)
    with open(path, "w") as f:
        f.write(f"spyro-1-dragons {poked} unknown unit\n")
    cli2 = subprocess.run([sys.executable, os.path.join(TREE, "scripts", "golden-fleet.py"), "run",
                           "--game", GAME, "--expect", path], capture_output=True, text=True)
    check("expect-refusals", len(errs) == 2 and cli.returncode == 2 and cli2.returncode == 2
          and "detached" not in cli.stdout + cli2.stdout,
          f"{len(errs)} refusals: {'; '.join(errs)}; `run --expect` exits {cli.returncode} (typo), "
          f"{cli2.returncode} (unknown unit), nothing detached")
    resumed = [rec("DIVERGE", poked, imported=True), rec("SAME", quiet), rec("END", "END")]
    c6, _ = gf.expect_counts([e_poked, e_quiet], {UNIT: (True, resumed, blessed)})
    c7, _ = gf.expect_counts([e_poked, e_quiet], {UNIT: (True, lines, blessed)})
    check("expect-resumed", c6 == c7, f"resumed {fmt(c6)} == uninterrupted {fmt(c7)}")

    co = FakeCo(tmp, "expect-log")
    co.expect = [e_poked]
    co.plan(UNIT, full_unit(div=[poked, TILES[2]], same=TILES[3:6], extra=[], ident=[], exceed=[]))
    job = gf.Job("unit", GAME, TIER)
    co.go([job])
    co.print_summary(1)
    tags = [ln.split()[-1] for ln in co.stream_log_lines()]
    lcount = {t: tags.count(t) for t in ("EXPECTED", "UNEXPECTED")}
    units = {UNIT: (True, job.atts[1].lines, blessed)}
    c8, _ = gf.expect_counts(co.expect, units)
    check("expect-log", lcount["EXPECTED"] == c8["EXPECTED"] == 1
          and lcount["UNEXPECTED"] == c8["UNEXPECTED"] == 1,
          f"stream.log tags EXPECTED {lcount['EXPECTED']}, UNEXPECTED {lcount['UNEXPECTED']}; "
          f"summary {fmt(c8)}")


class MoveCo(FakeCo):
    """A FakeCo with a second local host `b` (its own fleet root) already in the run; add_host admits it without a preflight."""

    def __init__(self, tmp, name):
        """Add host b beside the FakeCo host."""
        super().__init__(tmp, name)
        root = os.path.join(self.tmp, "root-b")
        host = gf.Host("b", {"transport": "local", "fleet_root": root})
        hs = gf.HostState(host, {"root": root, "engine": "podman"}, 1, self.run_id)
        os.makedirs(os.path.join(hs.run_dir, "ship"))
        shutil.copyfile(os.path.join(self.hs.run_dir, "ship", "supervise.sh"),
                        os.path.join(hs.run_dir, "ship", "supervise.sh"))
        self.hosts.append(hs)
        self.hb = hs
        self.skew["b"] = 0.0

    def add_host(self, name):
        """Admit a host of the run as is; refuse any other."""
        hs = self.host_state(name)
        if hs is None:
            raise gf.FleetError(f"{name}: unreachable: test host")
        return hs

    def plan_on(self, hs, job, body, attempt=None):
        """Write a job's plan into one host's ship directory."""
        suffix = f"-{attempt}" if attempt else ""
        with open(os.path.join(hs.run_dir, "ship", f"plan-{job}{suffix}.sh"), "w") as f:
            f.write(body)

    def request_move_later(self, delay, lines):
        """Write move-request lines after delay seconds."""
        def write():
            with open(self.move_path + ".tmp", "w") as f:
                f.write("".join(ln + "\n" for ln in lines))
            os.replace(self.move_path + ".tmp", self.move_path)
        threading.Timer(delay, write).start()


def case_move(tmp):
    """`move`: refusals, an accepted move carrying stamped rows to another host, a pinned re-queue outside the attempt budget, no ledger row."""
    n0 = len(ledger_rows())
    a, b = TILES[0:2], TILES[2:4]
    co = MoveCo(tmp, "move")
    co.hosts = [co.hs, co.hb]
    stamp = "".join(f"tile: 00 {t}\\n" for t in a)
    rows_a = "\n".join(f"emit SAME {t} 0123456789abcdef" for t in a)
    co.plan_on(co.hs, UNIT, f'mkdir -p "$UNIT_DIR/cand/resume"\n'
               + "".join(f'echo png >"$UNIT_DIR/cand/{t}"\n' for t in a)
               + f'printf "run: r\\n{stamp}" >"$UNIT_DIR/cand/resume/bk2.rowa.stamp"\n'
               + 'echo partial >"$UNIT_DIR/cand/unstamped-vk.png"\n'
               + f"{rows_a}\nsleep 60\n", attempt=1)
    check_rows = "".join(f'[ -f "$UNIT_DIR/cand/{t}" ] || finish 1 row-missing\n' for t in a)
    co.plan_on(co.hb, UNIT, check_rows
               + '[ -f "$UNIT_DIR/cand/resume/bk2.rowa.stamp" ] || finish 1 stamp-missing\n'
               + '[ ! -e "$UNIT_DIR/cand/unstamped-vk.png" ] || finish 1 unstamped-carried\n'
               + "\n".join(f"emit_imported SAME {t} 0123456789abcdef" for t in a) + "\n"
               + "\n".join(f"emit SAME {t} 0123456789abcdef" for t in b)
               + "\nemit CREDITS rc=0\nemit END\n"
               + "report 'verdict: PASS' 'exit: 0' 'compared: 4' 'credits-rc: 0'\nfinish 0 verdict\n",
               attempt=2)
    co.request_move_later(1.0, ["id=r1 unit=spyro-9-presentation to=b",
                                f"id=r2 unit={UNIT} to=nowhere",
                                f"id=r3 unit={UNIT} to=local",
                                f"id=r4 unit={UNIT} to=b"])
    job = gf.Job("unit", GAME, TIER)
    co.go([job], deadline_s=60)
    log = open(os.path.join(co.local_run_dir, "moves.log")).read().splitlines()
    verdicts = [ln.split()[1] for ln in log]
    keys = {gf.stream_key(x) for x in job.atts[2].lines} if 2 in job.atts else set()
    want = {gf.stream_key(gf.parse_stream_line(f"SAME {t} 0123456789abcdef 1.0"))
            for t in a + b}
    check("move", verdicts == ["refused", "refused", "refused", "accepted"]
          and job.result["rc"] == 0 and job.result["host"] == "b" and job.result["attempt"] == 2
          and job.moves == 1 and job.moved == [("local", "b")] and job.pinned == "b"
          and keys >= want and not job.result.get("stream_mismatch") and len(ledger_rows()) == n0,
          f"moves.log {verdicts}; result rc {job.result['rc']} on {job.result['host']} attempt "
          f"{job.result.get('attempt')}; moves {job.moves} {job.moved}; stream keys of attempt 2 "
          f"cover the unmoved tiles: {keys >= want}; ledger rows +{len(ledger_rows()) - n0}; "
          f"accepted line: {log[-1] if log else '-'}")
    co.request_move_later(0.0, [f"id=r5 unit={UNIT} to=local"])
    time.sleep(0.2)
    co.apply_moves([], None)
    last = open(os.path.join(co.local_run_dir, "moves.log")).read().splitlines()[-1]
    check("move-finished", "refused" in last and "has finished" in last, last)
    co.teardown()


def case_move_cli(tmp):
    """`golden-fleet.py move`: refused once done, unknown run; the request line and --no-wait."""
    root = os.path.join(tmp, "move-cli-root")
    run = os.path.join(root, "runs", "r-cli")
    os.makedirs(run)
    hosts = os.path.join(tmp, "move-cli-hosts.toml")
    with open(hosts, "w") as f:
        f.write(f'[hosts.local]\ntransport = "local"\nfleet_root = "{root}"\n')
    env = dict(os.environ, OPENPETE_FLEET_HOSTS=hosts)
    cmd = [sys.executable, os.path.join(TREE, "scripts", "golden-fleet.py"), "move"]
    unknown_host = subprocess.run(cmd + ["r-cli", "--unit", UNIT, "--to", "zz", "--no-wait"],
                                  env=env, capture_output=True, text=True)
    ok = subprocess.run(cmd + ["r-cli", "--unit", UNIT, "--to", "local", "--no-wait"], env=env,
                        capture_output=True, text=True)
    req = gf.read_move_requests(os.path.join(run, "move-request"))
    open(os.path.join(run, "done"), "w").write("rc=0\n")
    done = subprocess.run(cmd + ["r-cli", "--unit", UNIT, "--to", "local"], env=env,
                          capture_output=True, text=True)
    norun = subprocess.run(cmd + ["r-none", "--unit", UNIT, "--to", "local"], env=env,
                           capture_output=True, text=True)
    check("move-cli", unknown_host.returncode == 2 and ok.returncode == 0 and len(req) == 1
          and req[0]["unit"] == UNIT and req[0]["to"] == "local" and done.returncode == 1
          and "resume" in done.stderr and norun.returncode == 2,
          f"unknown host {unknown_host.returncode}, request {req}, done {done.returncode}, "
          f"no run {norun.returncode}")


def case_keep_and_resume_inputs(tmp):
    """A stopped unit's stamped rows and ship are kept at teardown; resolve_saved reads them; resume refuses a finished unit and an active run."""
    co = FakeCo(tmp, "keep")
    co.golden_fd = gf.try_golden_lock(co.hs.root)
    co.plan(UNIT, 'mkdir -p "$UNIT_DIR/cand/resume"\n'
                  f'echo png >"$UNIT_DIR/cand/{TILES[0]}"\n'
                  f'printf "run: r\\ntile: 00 {TILES[0]}\\n" >"$UNIT_DIR/cand/resume/bk2.k.stamp"\n'
                  f"emit SAME {TILES[0]} 0123456789abcdef\nsleep 60\n")
    os.makedirs(co.ship_dir, exist_ok=True)
    with open(os.path.join(co.ship_dir, "fields-spyro-2.txt"), "w") as f:
        f.write("head: x\n")
    co.stop_later(0.5, f"unit={UNIT}")
    job = gf.Job("unit", GAME, TIER)
    co.go([job])
    co.print_summary(co.run_rc())
    co.teardown()
    kept = os.path.join(co.local_run_dir, "kept")
    files = gf.stamp_files(os.path.join(kept, f"u-{UNIT}", "cand"))
    saved = gf.resolve_saved(kept, UNIT, co.local_run_dir)
    left = sorted(os.listdir(co.local_run_dir))
    check("keep-stopped", gf.row_counts(files) == (1, 1) and saved["fields"].endswith("fields-spyro-2.txt")
          and "kept" in left and "ship" not in left,
          f"kept rows {gf.row_counts(files)}, fields {saved['fields']}, run dir {left}")
    # The summary line names the stopped unit, so `resume` reads its last result.
    with open(os.path.join(co.local_run_dir, "coordinator.log"), "w") as f:
        f.write(f"[fleet] result {UNIT}: FAIL on local (rc 1, attempt 1, - s; stopped by user)\n"
                "[fleet] result spyro-3-presentation: PASS on b (rc 0, attempt 1, 9 s; verdict; census x)\n")
    res = gf.unit_results(co.local_run_dir)
    check("resume-results", res[UNIT][3] == gf.STOP_STATUS and res["spyro-3-presentation"][2] == 0,
          f"{res}")


def case_keep_exhausted(tmp):
    """A unit past its deadline on every attempt ends attempts exhausted; its last attempt's stamped rows are kept for `resume`."""
    co = FakeCo(tmp, "keep-exhausted")
    co.golden_fd = gf.try_golden_lock(co.hs.root)
    co.plan(UNIT, 'mkdir -p "$UNIT_DIR/cand/resume"\n'
                  f'echo png >"$UNIT_DIR/cand/{TILES[0]}"\n'
                  f'printf "run: r\\ntile: 00 {TILES[0]}\\n" >"$UNIT_DIR/cand/resume/bk2.k.stamp"\n'
                  'echo partial >"$UNIT_DIR/cand/unstamped-vk.png"\n'
                  f"emit SAME {TILES[0]} 0123456789abcdef\nsleep 60\n")
    os.makedirs(co.ship_dir, exist_ok=True)
    with open(os.path.join(co.ship_dir, "fields-spyro-2.txt"), "w") as f:
        f.write("head: x\n")
    job = gf.Job("unit", GAME, TIER)
    co.go([job], deadline_s=1)
    co.teardown()
    kept = os.path.join(co.local_run_dir, "kept", f"u-{UNIT}")
    files = gf.stamp_files(os.path.join(kept, "cand")) if os.path.isdir(kept) else []
    host = open(os.path.join(kept, "host")).read().strip() if os.path.exists(os.path.join(kept, "host")) else "-"
    unstamped = os.path.exists(os.path.join(kept, "cand", "unstamped-vk.png"))
    saved = gf.resolve_saved(os.path.join(co.local_run_dir, "kept"), UNIT, co.local_run_dir) if files else {}
    check("keep-exhausted", job.result["status"] == "attempts exhausted" and not job.stopped
          and job.result.get("attempt") == gf.MAX_ATTEMPTS and gf.row_counts(files) == (1, 1)
          and host == "local" and not unstamped and saved.get("host") == "local",
          f"result {job.result['status']} (attempt {job.result.get('attempt')}), kept rows "
          f"{gf.row_counts(files)}, host {host}, unstamped tile kept: {unstamped}")


def case_ledger(tmp):
    """Only a whole unit writes a ledger row; a filtered unit and an attempt that imported rows keep the deadline at the whole unit's time."""
    done = "report 'verdict: PASS' 'exit: 0' 'imported-rows: {n}'\nWALL={w}\nfinish 0 verdict\n"
    slow = gf.HostState(gf.Host("slow", {"transport": "local", "speed": 0.25}), {"root": "/nonexistent"}, 1, "x")
    n0 = len(ledger_rows())
    co = FakeCo(tmp, "ledger-whole")
    co.plan(UNIT, done.format(n=0, w=900))
    whole = gf.Job("unit", GAME, TIER)
    co.go([whole])
    n1 = len(ledger_rows())
    norm = float(ledger_rows()[-1].split("\t")[gf.LEDGER_COLS.index("norm_s")])
    co.ledger = gf.read_ledger()
    before = co.deadline(whole, slow)
    co = FakeCo(tmp, "ledger-filtered")
    co.a.filter, co.a.freecam_filter = "summer_forest", None
    co.plan(UNIT, done.format(n=0, w=200))
    filt = gf.Job("unit", GAME, TIER)
    co.go([filt])
    n2 = len(ledger_rows())
    co = FakeCo(tmp, "ledger-imported")
    co.plan(UNIT, done.format(n=3, w=300))
    imp = gf.Job("unit", GAME, TIER)
    co.go([imp])
    n3 = len(ledger_rows())
    co.ledger = gf.read_ledger()
    after = co.deadline(imp, slow)
    check("ledger", whole.result["rc"] == filt.result["rc"] == imp.result["rc"] == 0
          and n1 == n0 + 1 and n2 == n1 and n3 == n1 and imp.result.get("imported_rows") == 3
          and before == after == norm / 0.25 * 2,
          f"rows +{n1 - n0} whole, +{n2 - n1} filtered, +{n3 - n2} imported "
          f"(imported_rows {imp.result.get('imported_rows')}); deadline at speed 0.25 {before:.0f} s "
          f"before, {after:.0f} s after (norm_s {norm:.0f} / 0.25 x 2)")


def read_verdict(run_dir):
    """The `key=value` lines of a run directory's canary-verdict.txt as a dict; None when absent."""
    try:
        with open(os.path.join(run_dir, gf.CANARY_VERDICT), encoding="utf-8") as f:
            return dict(ln.rstrip("\n").partition("=")[::2] for ln in f if "=" in ln)
    except OSError:
        return None


def case_canary_verdict(tmp):
    """canary-verdict.txt per host and the pulled record's fleet stamp."""
    from PIL import Image
    pb = gf.load_pb(TREE)
    co = FakeCo(tmp, "canary-verdict")
    dirs = os.path.join(co.tmp, "canary")
    co.canary_dir = lambda h: os.path.join(dirs, h)

    def scenario(pixels, rcs):
        """Canary tiles per host (`pixels` maps host to a pixel value), leg rcs; return the verdict dict."""
        shutil.rmtree(dirs, ignore_errors=True)
        jobs = []
        for h, rc in rcs.items():
            os.makedirs(os.path.join(dirs, h))
            Image.new("RGB", (4, 4), (pixels[h], 0, 0)).save(os.path.join(dirs, h, "c__canon__1-vk.png"))
            j = gf.Job("canary", GAME, TIER, pinned=h)
            j.result = {"rc": rc, "status": "verdict" if rc in (0, 1) else "leg failed"}
            jobs.append(j)
        co.hosts = [co.hs] * len(rcs)
        rrc = co.compare_canaries(jobs)
        got = read_verdict(co.local_run_dir)
        return rrc, got

    rc_same, same = scenario({"a": 1, "b": 1}, {"a": 0, "b": 1})
    rc_diff, diff = scenario({"a": 1, "b": 2}, {"a": 0, "b": 0})
    rc_leg, leg = scenario({"a": 1, "b": 1}, {"a": 0, "b": 2})
    rc_one, one = scenario({"a": 1}, {"a": 0})
    first = lambda d, h: d.get(f"canary.{h}", "").split(" ")[0]  # noqa: E731
    ok_verdicts = (rc_same == 0 and same["run"] == co.run_id and same["canary"] == "on"
                   and first(same, "a") == first(same, "b") == "pass"
                   and rc_diff == 1 and first(diff, "a") == first(diff, "b") == "fail"
                   and first(leg, "b") == "fail" and first(leg, "a") == "fail"
                   and one["canary"].startswith("off") and "canary.a" not in one)
    check("canary-verdict", ok_verdicts,
          f"same {rc_same} {same}; diff {rc_diff} {diff}; leg {rc_leg} {leg}; one {rc_one} {one}")

    pull = os.path.join(co.tmp, "pull")
    os.makedirs(pull, exist_ok=True)
    with open(os.path.join(pull, gf.RECORD_NAME), "w") as f:
        f.write("game=spyro-2\nhost=remote-box\nfleet-run=stale\nimage-digest=sha256:i\n"
                "libvulkan=1.3\nrecipe-hash=r\n")
    co.hs.name = "b"
    co.stamp_record(pull, co.hs)
    recd = pb.read_record(pull)
    stamped = (recd.get("fleet-run") == co.run_id and recd.get("fleet-host") == "b"
               and recd.get("fleet-run-dir") == os.path.abspath(co.local_run_dir)
               and recd.get("host") == "remote-box"
               and open(os.path.join(pull, gf.RECORD_NAME)).read().count("fleet-run=") == 1)
    check("canary-verdict-record", stamped, f"record {recd}")


def case_run_candidate(tmp):
    """An accepted pull becomes the run's own promotable candidate in the stream directory."""
    from PIL import Image
    pb = gf.load_pb(TREE)
    co = FakeCo(tmp, "run-candidate")
    job = gf.Job("unit", GAME, TIER)
    pull, dest = os.path.join(co.tmp, "pull"), co.stream_dir(job)
    for d in (pull, os.path.join(pull, "identity"), os.path.join(dest, "diff")):
        os.makedirs(d)
    for name, px in (("a__canon__1-vk.png", 1), ("b__canon__1-vk.png", 2)):
        Image.new("RGB", (4, 4), (px, 0, 0)).save(os.path.join(pull, name))
    Image.new("RGB", (4, 4), (3, 0, 0)).save(os.path.join(pull, "identity", "c__ident__1-vk.png"))
    for name in ("verdict-report.txt", "capture-device.txt"):
        with open(os.path.join(pull, name), "w") as f:
            f.write(name + "\n")
    with open(os.path.join(pull, gf.RECORD_NAME), "w") as f:
        f.write(f"game={GAME}\ntier={TIER}\nhost=remote-box\n")
    co.hs.name = "b"
    co.stamp_record(pull, co.hs)
    # The stream pulled a's superseded-attempt capture and a tile the accepted pull lacks.
    Image.new("RGB", (4, 4), (9, 0, 0)).save(os.path.join(dest, "a__canon__1-vk.png"))
    Image.new("RGB", (4, 4), (8, 0, 0)).save(os.path.join(dest, "z__canon__1-vk.png"))
    with open(os.path.join(dest, "diff", "a__canon__1-diff.png"), "wb") as f:
        f.write(b"heat")
    co.keep_run_candidate(job, pull)
    shutil.rmtree(pull)  # the next capture clears the shared pull target
    rec = pb.read_record(dest)
    a_px = Image.open(os.path.join(dest, "a__canon__1-vk.png")).getpixel((0, 0))[0]
    tiles = pb.promote_select(dest, True, [])
    ok = (tiles == ["a__canon__1-vk.png", "b__canon__1-vk.png"] and a_px == 1
          and os.path.isfile(os.path.join(dest, gf.STREAM_ONLY_DIR, "z__canon__1-vk.png"))
          and os.path.isfile(os.path.join(dest, "identity", "c__ident__1-vk.png"))
          and os.path.isfile(os.path.join(dest, "diff", "a__canon__1-diff.png"))
          and os.path.isfile(os.path.join(dest, "verdict-report.txt"))
          and os.path.isfile(os.path.join(dest, "capture-device.txt"))
          and rec.get("fleet-run") == co.run_id and rec.get("fleet-host") == "b"
          and rec.get("fleet-run-dir") == os.path.abspath(co.local_run_dir))
    check("run-candidate", ok, f"tiles {tiles}, a pixel {a_px}, record {rec}, "
          f"entries {sorted(os.listdir(dest))}")


def main():
    """Run every case in a temporary directory with the ledger redirected."""
    tmp = tempfile.mkdtemp(prefix="fleet-stream-check.")
    gf.ledger_path = lambda: os.path.join(tmp, "ledger.tsv")
    try:
        for case in (case_framing, case_pool, case_stream_off, case_equality, case_extra, case_exempt,
                     case_kill, case_stop, case_stop_cli, case_stop_sync, case_reap, case_clean,
                     case_expect, case_canary_verdict, case_run_candidate, case_move, case_move_cli,
                     case_keep_and_resume_inputs, case_keep_exhausted, case_ledger):
            case(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if FAILS:
        print(f"[fleet-stream-check] FAIL: {', '.join(FAILS)}")
        return 1
    print("[fleet-stream-check] PASS: every case")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(line_buffering=True)
    sys.exit(main())
