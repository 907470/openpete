# Present-thread interleaving harness

`tests/sched/` runs the display handoff under a deterministic scheduler and
checks it on every interleaving up to a bound. The handoff has three
parties: the sim thread (eager production, borrow and return, inline
presents), the present thread (pick, render into a stage image, acquire,
present, flip wait) and GPU retirement (fences, present ids, the scene
ring's serial window). The harness runs the engine's own code for all
three, so a change to that code is tested as written. No second model of
the protocol exists to drift from it.

| Part | Code under test |
|---|---|
| Present thread | `core/runtime/present_thread.c`, compiled whole |
| Scene ring and display wrappers | 27 functions of `core/renderer/native_render.c`, copied by name by `tests/sched/present/extract_ring.py` |
| RHI present path | `core/rhi/rhi_vulkan.c`, compiled whole, over a fake Vulkan device (`tests/sched/present/fakevk.c`) |
| Compose | a stand-in for `render_frame_common` with its protocol calls under the same guards (`ring_epilogue.h`) |
| Clients | a sim-thread driver script of the calls eager, the screenshot and capture brackets and stop make (`harness.c`) |

Release objects are untouched. The protocol files are compiled a second
time for the harness with `-fsanitize=thread`. The scheduler's own
`__tsan_*` definitions (`lib/tsan_hooks.c`) turn each instrumented access
to a watched address into a yield, and libtsan is not linked. SDL's mutex,
condition, thread, atomic, tick and delay calls are replaced at link time
by `-Wl,--wrap` (`SCHED_SDL_WRAPS` in `tests/sched/CMakeLists.txt`). SDL is
not linked either, so a call the wraps do not cover is a link error.
`objcopy --globalize-symbol` makes `s_pt`, `s_staged`, `s_lat` and
`s_display_owner` readable by the harness. The harness is Linux only
(GNU ld `--wrap`).

## The scheduler

`tests/sched/lib/` runs one thread at a time; each wrapped call and each
watched access is a yield, where the scheduler picks the next thread.
Time is virtual: `op_now` and `SDL_GetTicksNS` read the scheduler's
clock, and a sleep or timed wait blocks until the clock reaches its
deadline. Three kinds of choice exist:

- a preemption: switching away from a thread that could continue;
- a deviation: a non-default data choice (a fake device outcome, a time
  advance while a thread is runnable, a spurious wakeup);
- the default: the running thread continues, else the earliest deadline
  fires.

`DFS` enumerates every schedule with at most `p` preemptions and `d`
deviations, the joint bound. Each schedule runs in a child forked from a
process that created the device once. `PCT` samples schedules by random
thread priorities with a few priority change points. A failing schedule
prints its choice string, and `--replay` reruns it byte for byte, trace
included. `sched_calibrate` (label `unit`) proves the scheduler on toy
programs: a lost update through an unwatched counter is missed and
through a watched one found; a lock-order deadlock and a missed signal are
found; the schedule count of two two-yield threads is exact; replay is
exact.

## Properties

Every property is checked after each step of each schedule, at each fake
device call, at each present, at each drain's return or at quiescence.

| Property | Fails when |
|---|---|
| P-OWNER | `rhi_seam_violations()` is nonzero (the RHI's own display-owner check), or the fake device sees two threads inside one externally synchronised object |
| P-CLAIM | a thread's device call is not preceded by its own claim for the current activation (present thread) or after the latest drain (sim thread) |
| P-ACQUIRE | `set_active(0)` returns with a swapchain image acquired and neither presented nor released, or an image is presented with new content by a thread that did not acquire it |
| P-PRESENT | an acquisition is presented twice, or one instant is presented twice across ring scenes and inline presents |
| P-RETIRE | a ring slot is freed, or a FREE slot is published into, while the fake GPU still executes the submission that read it |
| P-ACCOUNT | at quiescence a published scene is not in exactly one bin (presented, superseded, refused at render, refused at present, discarded, dropped late), a slot is PENDING or PRESENTING, or the ring census disagrees with the harness's bins |
| P-DRAIN | a drain takes time outside the present thread's busy brackets, its own release and charged advances; or a schedule deadlocks; or a schedule passes its horizon inside a drain |
| P-SERVE | a pending scene waits more than 5 ms for a pick while the present thread is active, nothing is staged and the surface is presentable; a refused acquire restarts the window |

## Scenarios

| Scenario | Drives |
|---|---|
| S1 | a borrow, inline present and return while the present thread sleeps unlocked toward a staged scene's instant, then a publish under the new activation; twice |
| S1b | S1 with no inline present: the sim makes no device call while it holds the display |
| S2 | a drain while an acquired image is held for a staged scene far ahead |
| S3 | a resize seen by the compose on both threads, a minimised window, a display change from the pump thread, out-of-date and suboptimal outcomes, a borrow |
| S4 | ring pressure on a ring of 3 (`present_sched_r3`): supersede at pick and ring-full refusal |
| S5, S5u | the guest-discontinuity flush concurrent with pick and render; S5u with `OPENPETE_PT_STAGED=0` |
| S6 | stop while the present thread waits on a flip or sleeps |
| S7 | device loss offered at every modelled call |
| S8 | acquire-first order after a refused present, across a borrow |
| SR | (soak only) 48 client actions drawn by uniform choices: publish with strictly increasing targets, sleep, borrow with inline present, disengage and engage, flush, resize, minimise |

Each scenario runs in both present-timing configurations (`OPENPETE_PRESENT_TIMING`
0, the shipped default, and 1). Every scenario has a virtual-time horizon.
Passing it fails the schedule as a livelock; it is never a truncation.

## Running

| ctest | Label | Runs |
|---|---|---|
| `present_sched` | `unit` | S1-S8 exhaustively at each scenario's bound, both timings, both ring sizes; the engagement verdict; then the negative controls; about 45 s at 16 jobs |
| `present_sched_soak` | `slow` | PCT over every scenario and SR, 10 minutes, one worker per job, base seed from the date |
| `present_sched_tsan` | `slow` | the race-detector build: PCT over every scenario and SR for 60 s under ThreadSanitizer, then the control M10, which must report its race |
| `sched_calibrate` | `unit` | the scheduler's calibration |
| `sched_seams`, `sched_seams_selftest` | `fence` | the upkeep fence and its fixtures |

```
ctest --test-dir build/linux-release -R '^present_sched$' --output-on-failure
build/linux-release/tests/sched/present/present_sched --only S2 --timing 0 --out /tmp/pt
```

`--only S` and `--timing T` select one scenario and configuration; `--p`
and `--d` override the bound and `--max-schedules` caps the count. The run
prints one line per scenario: schedules, bound reached, steps per
schedule and wall time. The engagement report lists hits per seam site
and evaluations per property. A site never reached or a property never
evaluated makes the run VACUOUS, and the run fails.

### Replaying a failure

A failure prints the property, its message, the bound pair at which it was
found, the choice string (`choices 0,0,2,...`) and the trace file
`<out>/present_sched-<scenario>-timing<T>.trace`. The trace has one line
per step: step, virtual time, thread, operation, object and code address.

```
present_sched --only S1 --timing 0 --replay 0,0,0,2 --out /tmp/pt
```

reruns that schedule alone and rewrites the trace. A soak failure prints
its scenario, timing and seed. Replay it the same way with the choice
string printed above that line.

## Race-detector build

A thread runs without interruption between two yields, so the scripted
build interleaves only accesses the address filter watches. An unlocked
access the filter misses is never interleaved, and the scripted build
cannot see its race. `present_sched_tsan` closes that population with
ThreadSanitizer's happens-before detector.

It links the same harness objects, scenarios, fake device and
instrumented protocol objects as `present_sched`. The scheduler links
`lib/tsan_noise.c` in place of `lib/tsan_hooks.c` (`openpete_sched_noise`
in place of `openpete_sched`), and libtsan is linked:

- The instrumented code calls libtsan, which records every access; no
  access is a yield.
- The scheduler still runs one thread at a time. Its baton and its own
  state are hidden from the detector (ignore annotations), so the baton
  orders nothing the detector sees. The model mutexes, thread start and
  join, and the SDL atomics publish their happens-before edges
  (`sched_tsan_acquire`, `sched_tsan_release`). C11 atomics reach libtsan
  directly.
- The detector is happens-before based. It reports two accesses no edge
  orders whether or not they ran at the same moment. The PCT seeds vary
  which lock-order edges each schedule makes, and a race is replayed by
  its choice string like any failure.

Each report is classified in libtsan's report hook. A report is counted
and suppressed when the innermost frame of one of its accesses lies on a
source line whose trailing comment carries `sched: once` (an idempotent
first-use cache) or `sched: unordered` (a statistic no protocol decision
reads). Any other report is printed with both stacks and fails the
schedule with `TSAN: data-race: ...`. A `sched: owned` access is ordered
by a handoff the detector sees, so a report on it is a defect, not a
class. The run prints the report count and every classified site.

The kernel's address-space randomisation can exceed the sanitizer's
shadow layout. The ctest runs the binary under `setarch <arch> -R`, and so
does a manual run:

```
setarch "$(uname -m)" -R build/linux-release/tests/sched/present/present_sched_tsan \
  --out /tmp/pt --soak 60 --tsan-control build/linux-release/tests/sched/present/mutants/present_sched_tsan_M10
```

M10 (`mutants/M10.patch`) removes the lock around `set_active(1)`'s
writes of `s_pt.active` and `s_pt.activation`. Its binary,
`present_sched_tsan_M10`, runs the same soak with one worker and
`--expect-race`, and passes only on a schedule failing with an
unclassified race. A noise run whose control goes undetected is VACUOUS.
The target builds where the toolchain links `-fsanitize=thread`
(`OPENPETE_SCHED_HAVE_TSAN`).

## Negative controls

Each control is a unified diff in `tests/sched/present/mutants/` (a
description line, then the diff) against `present_thread.c` or
`native_render.c`. At build time `apply_mutant.py` applies it to a copy,
and the copy is linked into `build/.../present/mutants/present_sched_<M>`.
The context must match exactly; only the line offset may move. A patch that
no longer applies fails the build with the mutant's name. `present_sched
--mutants DIR` runs each control after HEAD with only its expected
properties armed. A disarmed property's violation does not stop the
schedule; it is appended to the failure message as `[disarmed P-X fired
first, at step N]`. The suite fails when a control is undetected, or when a
property detects no control (VACUOUS).

| Control | Mutation | Scenario | Armed |
|---|---|---|---|
| M1 | claim keyed on a flag reset only when the thread sees `active == 0` | S1 | P-OWNER, P-CLAIM |
| M1b | M1 | S1b | P-CLAIM |
| M2 | ring slots retire one render serial early | S6 | P-RETIRE |
| M3 | the drain's stage release removed | S2 | P-ACQUIRE |
| M4 | the drain's wait for the busy bracket removed | S2 | P-OWNER |
| M5 | the drain releases before it claims | S3 | P-OWNER |
| M6 | the drain's ring flush removed | S1 | P-PRESENT |
| M7 | the `presented` census increment removed | S1 | P-ACCOUNT |
| M8 | the `idle_cv` signal leaving a busy bracket removed | S2 | P-DRAIN |
| M9 | the kick's signal removed and the idle wait made untimed | S8 | P-SERVE |

## Changing a protocol file

A change to `present_thread.c`, the copied ring functions,
`render_frame_common` or the RHI present path keeps the harness current
in the same change set:

1. Rebuild. A mutant whose context the change moved fails the build:
   regenerate its patch from the new source (apply the mutation by hand,
   `diff -U3`, keep the description line). Confirm the control is still
   detected.
2. `present_sched_seams` (in `ALL`) runs `scripts/fences/sched_seams.py`:
    - Wraps: every synchronisation, thread, atomic or clock symbol the
      instrumented objects reference is wrapped or in `STUBBED`. A new SDL
      call goes into `SCHED_SDL_WRAPS` with its `__wrap_` definition in
      `lib/sdl_wraps.c`.
    - Atomic census: every `_Atomic` lvalue and SDL atomic call in
      `present_thread.c`, `rhi_vulkan.c` and the ring copy has a row in
      `ATOMICS`. A `watched` row's address is registered with the scheduler
      in `config_setup` (`harness.c`); an `excluded` row is excluded there
      by `sched_watch_exclude`.
    - Extraction: `extract_ring.py` refuses a copied function that is
      missing or defined twice. A new helper a copied function calls is a
      compile error naming it: stub it in `ring_prelude.h`.
    - Compose stand-in: `render_frame_common`'s protocol calls,
      `s_last_frame_presented` stores and returns, each with its enclosing
      `if` and `?:` conditions, must equal the stand-in's. Update
      `ring_epilogue.h`, or `STANDIN_SUBST` for a guard the stand-in
      replaces with a scenario variable.
    - Source seams: the protocol files carry no `op_sched` or `OP_SCHED`
      token.
    - Clients: a file that calls `openpete_present_thread_set_active`,
      `_borrow` or `_return`, or joins `seam_guard.py`'s `RHI_ALLOW`, is
      modelled by the driver and listed in `CLIENTS` with the call
      sequence it models.
3. Run `present_sched` and, at the end of the change, `present_sched_soak`
   and `present_sched_tsan`.

### Adding a seam or a stub

- An SDL synchronisation call: add its name to `SCHED_SDL_WRAPS` and a
  `__wrap_` definition in `lib/sdl_wraps.c` that calls the scheduler's
  primitive (`sched_mutex_lock`, `sched_cond_wait`, ...).
- A shared variable the threads race on: register its address in
  `config_setup` with `sched_watch` (every access yields),
  `sched_watch_guarded` (accesses yield unless the guard mutex is held) or
  `sched_watch_owned` (a predicate names the owning thread). Then add its
  `ATOMICS` row when it is atomic.
- A Vulkan entry point the RHI starts calling: `gen_vk_list.py` gives it a
  trap from `nm -u` automatically. Model its behaviour in `fakevk.c` when
  the present path reaches it.
- A seam site the engagement report must see: add a `pt_site` value in
  `pt_harness.h`, its name in `k_site_names`, and a classification rule in
  `site_rules` or a hit in the fake device hook.

## Limits

- Interleavings are sequentially consistent. Relaxed-atomic reorderings
  are not explored; the census classes every relaxed atomic as
  excluded or outside the protocol.
- Coverage is bounded by `(p, d)` per scenario and by the PCT budget. It
  is not a proof.
- The compose stand-in and the client driver are models, pinned by the
  fence. The locks the recording takes between the begin and end of a
  render (mod registry, VRAM, terrain cache service) are not modelled.
- The harness does not run the fullscreen toggle (a window operation
  routed through the host loop); `seam_guard.py` rules B and C fence it.
- The fake device allows one outstanding acquire per swapchain, the RHI's
  own invariant.
- The race-detector build orders threads through the scheduler's model
  mutexes and conditions; SDL's own implementations are not run.
