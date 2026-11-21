# Bug reports

A bug report is one zip a reader with this repository debugs from without
the tester's machine. The `bug_report` key action (default ++f9++, every
build) or `OPENPETE_BUGREPORT_AT`
writes it under the data directory's `bugreports/`; a hang-watchdog trip
or a CdSync spin-guard exit writes one through the exit flush. The User
side is [Reporting a bug](../user/help/reporting-a-bug.md). This page
covers reading one, replaying it, and the variables that drive the writer
from a harness.

## Bundle members

`manifest.json` (schema 2; readers accept 1 and 2) lists every member with its size and SHA-256,
and the identity fields: build version and commit, exe SHA-256, module
SHA-256 and march, codegen and ROM stamps, disc name and SHA-256, game,
argv, the anchor and press records (frame, VBL, feed key, park site), the
taint counts, the thumbnail list, the sub-tick ring's counts, the note
state and the redaction counts with their findings, the members left out
and the members shipped flagged (schema 2). Its `records` field
names the performance records the writing build records: the pace record
types (`pace.types`), whether GPU_FRAME records were on
(`pace.gpu_frames`), and the coarse ring's member, version, row count and
row span (`pace_coarse`). A bundle without `records` was written before
these records existed; the reader still reads its pace ring.

| Member | Content |
| --- | --- |
| `anchor.state`, `card<N>.mcd`, `card<N>.mcd.modsave` | the recorder's anchor: a savestate image 30 to 60 s before the press, with the card images and mod save records it held |
| `press.state`, `press-card<N>.mcd`, `press-card<N>.mcd.modsave` | the state at the press; absent in a fatal-exit bundle |
| `segment.bk2` | the input-log rows from the anchor's consume point to the press |
| `hashes.csv` | one `frame,vbl,fnv1a64,<region>...` row per tick from the anchor, the `--gameplay-hash-log` row |
| `host-trace.bin`, `pace.oppl`, `input-events.csv`, `hang/rings.bin` | the host-loop trace, the pace ring (last 30 s), the raw host input ring, the hang watchdog's dispatch rings |
| `screen.png`, `thumbs/NNN.png` | the presented image at the press; the thumbnail ring, oldest first |
| `pace-coarse.bin` | the coarse session ring: one row per second since the pace ring was armed (rows merge pairwise and the span doubles when 8192 rows fill), folded from every pace record: VBLs and the not-live ones, presents, repeats, sheds, slips, the longest present interval, the TICK_SPLIT sums, the EAGER_PLAN slots, scenes, cuts and highest level, the GPU_FRAME sum and maximum (layout in `core/include/openpete_pace_log.h`, `OP_PACE_CR_*`) |
| `subtick.bin` | the sub-tick present ring: the last 1024 re-extracted presents (2.13 s at 480 Hz) with kind, alpha, the tick stamps they lerp between and the scene ring slot, and per drawn object record its pairing key, class, pairing verdict, the snapshot pair's prev and curr positions, angles and roll stamps, and the screen centroid and world position it was drawn at (layout in `core/include/openpete_bugrec_subtick.h`) |
| `audio-feed.txt` | the audio device feed at the press: mute and device state, its counters (the exit summary's `spu:` and `feed:` lines and a `feed: now:` state line), and its event ring of the last 2048 events (layout under [Audio feed record](#audio-feed-record)) |
| `reports.txt`, `pace-census.txt`, `summary.txt` | the subsystem reports at the press and at the screenshot; their pace lines; the state summary |
| `config.txt`, `keymap.txt`, `config-files/`, `env.txt`, `argv.txt` | `--config-report`, `--keys-report`, the raw config tiers, the `OPENPETE_*` environment, the command line |
| `knobs.txt` | every knob registry row at the press with its boot value, default and flags, and each change of a live row after boot with its tick (layout under [Knob record](#knob-record)) |
| `warps.txt` | each warp request with the tick its queue drained (frame, VBL, feed key, route, target): the dev-overlay level and area warps (`warp_go`, `warp_area_go`, target the level or area number) the Mods menu's custom-level warp (`custom_warp`, target the row's position in name order) and its slot/replace toggle (`custom_mode`, target the row's position times 2, plus 1 for replace), which the replay queues again at that tick; a mod's level entry (`mod_level_enter`, target the level id), which the replay matches against the mod's own call. The replay's `warps:` line counts the requests taken, taken past their frame, and not reached |
| `device.txt`, `devices.txt` | Vulkan device, displays, system; the input devices |
| `log.txt`, `log-prev.txt`, `hang/`, `crash/` | the log tails of this run and the previous one, the hang dumps and minidump the previous run's log names |
| `mods.txt`, `mods/<id>/` | every installed mod with per-file SHA-256; every enabled mod's files, its loaded object and its effective config rows; a link inside a mod's tree is skipped, never followed |
| `module.txt`, `module/<file>` | the recomp module's linkage, stamps and march; the module itself when it is a file |
| `notes.txt` | the tester's note, when one was saved |

### Redaction

The redactor's identities: the home directory as `$HOME` (`%USERPROFILE%`
and `%HOMEDRIVE%%HOMEPATH%` on Windows) gives it, resolved through links,
with either separator and, on Windows, its 8.3 short and long names; the
user name from `$USER`, else `$LOGNAME` (`%USERNAME%`), else the home
directory's last component; the host names from the uname nodename,
`%COMPUTERNAME%` and `$HOSTNAME`, each with its first label when dotted.

Text members (`manifest.json`, every `.txt` member, `input-events.csv`,
the raw config files, `mods/<id>/config.toml` and every entry of
`segment.bk2`) are rewritten:

- a token starting with a home form becomes `~`; `$XDG_DATA_HOME`,
  `$XDG_CONFIG_HOME`, `$XDG_STATE_HOME`, `$XDG_CACHE_HOME`,
  `%LOCALAPPDATA%` and `%APPDATA%` outside home become their variable's
  name; any other absolute path keeps its last component only;
- the user name and the host names become `<user>` and `<host>`, in any
  letter case, wherever one stands as a whole word: bounded by bytes that
  are not ASCII letters or digits, `_`, `-` or UTF-8 bytes, so
  `user@host` and `host: name` are replaced and a longer word holding a
  name is not;
- a name shorter than 3 bytes is replaced only as a whole path component
  or a whole `key=` value, since a one- or two-byte word occurs in
  ordinary text; the scan below does not look for it.

The state files' card paths are rewritten to `card1.mcd` and `card2.mcd`.

Every rewritten member is scanned again for the home forms and the names
of 3 bytes or more. A member is left out of the bundle, listed in
`redaction.dropped` with the reason (`dropped_total` is the exact count)
and logged as `bugreport: left out`, when its redaction runs out of
memory, an identity survives it (a home form inside a longer token), its
bk2 does not parse, its state image does not parse (the card paths cannot
be rewritten), its name holds an identity, or it is the Windows minidump
(`crash/prev-openpete-crash.dmp`) and the scan finds any identity in it.
A minidump holds process memory (module paths, the environment), so it
holds the home directory whenever the program runs from inside it; the
replay does not read it. A bundle missing one member
is still a usable report, so a member is dropped rather than the bundle
refused; `manifest.json` cannot be dropped, so a manifest whose redaction
fails or whose scan matches refuses the bundle (`BUG REPORT FAILED`).

Members never rewritten are scanned: the state files beyond their card
paths, card images, `.modsave` records, `hashes.csv`, `host-trace.bin`,
`pace.oppl`, `pace-coarse.bin`, `subtick.bin`, `hang/`, the PNGs, the
recomp module and every file of an enabled mod. Each match is a
finding in `redaction.list` (the first 256; `findings` is the exact count)
logged as `bugreport: finding:`. A member with a match ships unchanged,
listed in `redaction.flagged` with its counts by kind (`flagged_total` is
the exact count) and logged as `bugreport: shipped <member> unchanged
with identity matches`. A name match in binary data can be a coincidence
of bytes; a home match is a path. The state files, card images and host
records ship flagged rather than dropped, since the replay needs them.
When any member ships flagged, a saved bundle posts a second toast beside
`BUG REPORT: SAVED`, held 15 s, and logs the same line as a warning:
`Some files in this report contain your user folder or computer name:`
followed by the flagged members' names, cut to `, and N more` past 200
bytes. A bundle written at a fatal exit logs the line only.

The recomp module and compiled mod objects are built with
`-ffile-prefix-map`, so neither carries a directory of the build machine:
every CMake preset maps the source tree to `openpete` and the build tree
to `openpete-build`; the first-boot module build maps the working
directory to `.`, the install directory to `openpete` and the module's
build directory to `module`; a mod compile (at boot, `--compile-mods`,
the cross presets' mod DLL rule and `scripts/crossbuild-mods.sh`) maps the
working directory to `.`, the compiler's install tree to `toolchain`, the
SDK to `sdk` and the mod's directory to `mods/<id>`. A module or prebuilt
mod object built elsewhere without these remaps ships flagged.

A mod config row marked `secret = true`, and every key the mod's schema
does not declare, is written as a comment naming the key.

What the bundle records about the machine and the tester:

| Fact | Member | Redaction |
| --- | --- | --- |
| command line | `argv.txt`, `manifest.json` `argv` | text rules |
| every set `OPENPETE_*` variable | `env.txt` | text rules |
| resolved settings, raw `openpete.toml` tiers, key bindings, knob values | `config.txt`, `config-files/`, `keymap.txt`, `knobs.txt` | text rules |
| log tails | `log.txt`, `log-prev.txt` | text rules |
| OS name, kernel release and version string, architecture, CPU model and count, RAM, `/etc/os-release` name and version, WSL flag | `device.txt` | text rules; no host name is read for it |
| session type, desktop, SDL drivers, Steam Deck flags, `WSL_DISTRO_NAME`; whether `DISPLAY`, `WAYLAND_DISPLAY`, `GAMESCOPE_WAYLAND_DISPLAY` are set | `device.txt` | text rules; display values are not recorded |
| GPU name, vendor and device ids, driver name, info and version, extensions, swapchain | `device.txt`, `reports.txt` | text rules; no device UUID or LUID |
| display names (the monitor's model), modes and scales; audio drivers and format | `device.txt` | text rules; no EDID serial |
| controllers: vendor and product ids, SDL GUID, name and mapping when in SDL's database | `devices.txt` | names outside the database withheld; keyboard and mouse names withheld; the SDL GUID holds bus, vendor, product, version and a name CRC, no serial |
| disc | `manifest.json` `disc`, `disc_sha256` | the file name only |
| executable | `manifest.json` `exe_sha256` | the hash only |
| recomp module path | `module.txt` | text rules |
| mods: ids, versions, relative file paths and hashes, enabled mods' files and objects, declared non-secret config values | `mods.txt`, `mods/` | `mods.txt` and `config.toml` by text rules; the files are scanned only |
| tester's note | `notes.txt` | text rules |
| card paths | `*.state` | rewritten |

The bundle records no network address, MAC address, machine or disk
serial, or account id. The rules do not replace a real name, an e-mail
address, another account's name, or a short name outside a path.

## Performance records

The pace ring (`pace.oppl`, 30 s) carries three record types beside the
pacing records, and `pace-coarse.bin` folds every record into rows that
cover the whole session.

| Record | When | Content |
| --- | --- | --- |
| `TICK_SPLIT` (15) | after each VBL whose fiber run raised a present (a drawing VBL) | microseconds since the previous one: sim (every fiber run), extract (re-extracts, as in PRESENT_SPAN), terrain (the canonical terrain build and scene feed, inside sim), moby (the canonical moby native pass after the game's draw, inside sim), snap in sub-tick re-extracts (inside extract) and in the canonical feed (inside terrain and moby), render (scene render and submit on the presenting thread), present (the present thread's staged present: acquire, blit, queue present); the counts of extracts, renders, presents and snap resolves; the drawing VBL's own fiber run |
| `EAGER_PLAN` (16) | each span eager production plans (one per canonical tick, windowed only) | the `OPENPETE_EAGER_DBG` plan line: slots, budget, per-scene tail cost, afford, the span's level and the standing level, the previous span's cut, lag, non-drawing VBL cost, tick start lateness, planned scenes and the planned grid period |
| `GPU_FRAME` (17) | each rendered frame's harvested GPU timestamp zones, under `OPENPETE_BUGREPORT_GPU=1` | first zone start to last zone end, the depth-0 zone sum, the zone count and the RHI frame token |

Headless runs write TICK_SPLIT records with the sim, terrain, moby and
canonical snap slots (and only when the native renderer is up, which
headless needs a `--screenshot-on-frame` for); eager production, and with
it EAGER_PLAN and the extract, render and present slots, runs only in a
window. `OPENPETE_BUGREPORT_GPU` is off by default: its GPU cost on a weak
GPU is unmeasured. Measure it on the Windows exe with two windowed
`--pace-log` runs of the same bk2, with and without the variable, and
compare the present-interval p99 and the fence block times
(`scripts/pacing/pace.py`).

Sizes: a TICK_SPLIT and an EAGER_PLAN record are 64 B each per tick, 3.8
KB/s together (115 KB in a 30 s ring); GPU_FRAME is 64 B per rendered frame
(115 KB per 30 s at 60 Hz, 920 KB at 480 Hz). They share the existing 8 MiB
ring (131072 records), which then holds 30 s at about 2600 records per
second. The coarse ring is 1 MiB in memory (8192 rows of 128 B) and
writes only its used rows: 450 KB raw for an hour.

## Audio feed record

PsyCross's device feed (`third_party/PsyCross/src/audio/PsyX_SPUFeed.cpp`)
records every timeline event, the events `OPENPETE_AUDIO_FEED_LOG` writes,
into a ring of the last 2048 (`PSYX_SPUFEED_EVENTS`) whether or not the log
is open: `tick` (a sample every 250 ms of device time; the feed log's name,
not a sim tick), `phase`, `hold`, `release`, `prime`, `UNDERRUN`, `drain`
(data ran out while held), `OVERFLOW`, `overflow-end`, `lag-drop`,
`step-drop`, `stretch`, `stretch-end` and `slip` (the host's slip count
rose).

Threads: the event is recorded by the thread that raises it, under the
feed's mutex, which that thread already holds: the audio consumer thread
(`audio` role) for samples, primes, underruns, drains, drops, holds,
releases, stretch edges and slips; the sim thread (the producer) for
overflows and phase changes. OpenAL's mixer thread never touches the feed.
The press reads the ring on the sim thread without a lock: each 112 B slot
carries a sequence word (odd while written, even naming its event), and a
slot rewritten during the copy is skipped and counted
(`events_skipped`). Recording stores 14 words and reads the clock; it
allocates and prints nothing. The counters are read under the feed's
mutex (the exit summary's call), except at a fatal exit off the sim's
thread, where the file says `counters=not read`.

Sizing: over 51 feed logs of earlier audio work (15 to 218 s each),
non-sample events run at 1 to 10 per minute in steady play; the worst 60 s
window held 464 (an overflow starting and ending at each push under an
injected stall) and the worst 5 s window 159. With the 4 samples per
second the ring covers 8.5 min of steady play, 2.9 min at the worst 60 s
rate and 57 s at the worst 5 s rate sustained. The ring is 229 KiB of
memory; a full ring is about 195 KB of text (95 B per row).

`audio-feed.txt` (version 1): `key=value` header lines (`version`, `mute`,
`device` openal, synthetic or none, `snapshot_wall_ns`,
`snapshot_present_ns`, `snapshot_vbl`, `counters`, `ring`, `events_total`,
`events_held`, `events_skipped`, and `note` lines stating a mute or a
missing device); the counter lines; then one row per event, oldest first:

| Column | Content |
| --- | --- |
| `t_s` | seconds before the snapshot (`SDL_GetTicksNS`, the events' clock) |
| `vbl` | the host's VBL count the feed last received (`openpete_vbl_device`) |
| `consumed` | frames the device had consumed: the device clock |
| `event`, `phase` | the event name; the host's phase (`boot`, `skip`, `unlocked`, `paused`, `live`, `headless`) |
| `flags` | `h` held, `p` data playing, `s` time stretch engaged; `-` when not |
| `ratio` | input frames per output frame; the reader shows it in cents |
| `speed`, `drift` | the measured producer speed against real time; producer-time frames per device frame |
| `depth_s`, `st_rate`, `fifo` | the smoothed queued depth in output frames; the stretch rate; FIFO frames |
| `underruns`, `slips`, `behind` | underruns so far; the host's slip periods; sim periods the producer is behind real time |
| `pushed` | frames the producer had pushed |
| `value` | `lag-drop`, `step-drop`: output frames dropped; `slip`: slip periods added; `prime`: the depth target; `OVERFLOW`, `overflow-end`: frames dropped on overflow so far; `hold`, `release`: frames discarded so far; else 0 |

A muted run (`--mute 1` or the overlay) still runs the feed on the
silenced SPU output; `mute=1` and a `note` line say so. A run whose audio
device did not open holds only `phase` events; `device=none` and a `note`
line say so.

## Knob record

`config.txt` is the boot decision table: the value each knob took at boot
and the config layer it won at. A knob changed after boot, through the dev
overlay, the settings menu or a hotkey, is in no member but `knobs.txt`.
Mods have no call that sets a host knob; a mod's own config is in
`mods/<id>/`.

The recorder samples the registry on every committed tick after fast
boot, on the sim thread: at the first tick it records every row's value as
its boot value (the knob registry is initialized there when no window ran
the overlay's init); after it, it reads each live row's getter (a numeric
row's number, a string row's text, the `OPENPETE_*` rows only when the
registry's environment epoch moved) and records each change with the tick,
VBL, feed key and seconds since SDL init. Reading the getters covers every
route a value moves by. The press samples once more. The newest 256
changes are kept; the `session` line counts the dropped ones.

`knobs.txt` (version 1) is tab-separated, one record per line, `#` lines
are comments:

```
session first_frame=1 changes=3 kept=3 dropped=0
knob    <id> <class> <value> <boot> <default> <source> <flags> <raw> <changes> <last frame@s>
change  <frame> <vbl> <feed_key> <t_s> <id> <old> <value> <old_raw> <raw>
```

`value`, `boot` and `default` use the flag's words where the row has a
flag (else the overlay's); `default` is `(decided: ...)` for a row whose
default the program decides. `source` is the layer the boot value won at,
the `won at` column of `config.txt`. Flags: `L` the value differs from the
boot value, `D` it differs from the declared default, `C` the row changed
after boot (`C` without `L`: it changed back). `raw` is the number the
row's setter takes (`-` for a string row). Action rows (buttons) have no
line. The member is a text member: a path value is redacted by the rule
above (home to `~`, a path outside home to its basename) and the user and
host names as whole words; no other knob value holds a device or personal
name.

The class decides what `--bug-report-replay` does with a change:

| Class | Rows | Replay |
| --- | --- | --- |
| `render` | every live row not named below: renderer state, which a live row's setter alone may write | applied |
| `host` | `fullscreen`, `vsync`, `render_fps`, `tick_scale`, `runahead`, `rewind`, `mute`, `sfx`, `music`, `vibration`, `mouse`, `pad_unfocused`, `mouse_sensitivity`, `log_level`, `log_format`, `save_mismatch_toast`, `record_fps_overlay`, `state_slot` | not applied: the replay's own headless, muted, flat-out run decides window, pacing, audio, input and logging |
| `sim` | `freecam`, `camlock`, `pause` | not applied: they act on the sim's timeline (the free camera freezes the player update, the camera lock writes the camera, pause stops the ticks); the movie and the hash compare cover the run |
| `latched` | every restart-only row | not applied: read at boot, and the replay boots on the tester's argv, config and environment; its value is its boot value |
| `env` | the `OPENPETE_*` variable rows | not applied from `knobs.txt`: `env.txt` holds the press value, set from the replay's start |
| `follow` | `warp_level` | not sampled after boot: the level warp's selection follows the player's level |

At the anchor load the replay sets each `render` row that changed after
boot to its value at the anchor (the old value of its first change after
the anchor, else its press value) through this build's setter, when that
differs from its boot value; each change after the anchor is applied at
the replayed tick equal to its recorded tick. The recorder sees a change
at the first sample after the setter ran, and a renderer setter's value is
latched at the next tick's start in both runs, so an applied change can
land one tick later than the tester's. A change of another class is
printed as not applied with its reason. The lines on stderr:

```
bug-report-replay: knob <id> = <value> applied at the anchor load (frame F; set live before the anchor; boot value B)
bug-report-replay: knob <id> = <value> applied at frame F (changed live at frame F, T s, from OLD)
bug-report-replay: knob <id> = <value> not applied (<class>: <reason>)
bug-report-replay: knobs: N changed after boot, A applied (L at the anchor load, T at their tick), M not applied
bug-report-replay: knobs: the bundle has no knobs.txt (written before the member): ...
```

A bundle written before the member replays as before, on the tester's
boot configuration. When the ring dropped changes after the anchor, the
replay is labelled `knobs-partial`. A live row writes renderer or host
state only, so an applied change moves the picture and not the gameplay
hashes; a change that did move them would show as DIVERGED.

## Reading a bundle: `tools/bugreport.py`

The reader treats a bundle as untrusted input: members are read by name
with size bounds; absolute names, `..` components, drive letters, NUL
bytes, duplicates and symlinks are refused. Everything it prints passes
through a filter that writes control characters (other than tab and
newline) and bidirectional overrides as `\xNN` or `\uNNNN`, so a log tail
or manifest field cannot drive the terminal. It uses the standard library
only and runs under `python3 -I`.

```
python3 -I tools/bugreport.py info BUNDLE [--log-lines N] [--members] [--archive DIR]
python3 -I tools/bugreport.py extract BUNDLE DIR
python3 -I tools/bugreport.py replay BUNDLE [--exe PATH] [--disc CUE] [--archive DIR]
                                     [--work DIR] [--timeout S] [--dry-run] [-- EXTRA...]
python3 -I tools/bugreport.py subtick BUNDLE [--class C] [--key K] [--last N] [--max-keys N]
                                      [--tol-px PX] [--tol-world W]
python3 -I tools/bugreport.py audio BUNDLE [--kind K,...] [--last N] [--no-ticks]
python3 -I tools/bugreport.py perf BUNDLE [--rows]
python3 -I tools/bugreport.py corpus PATH... [--tsv OUT] [--quiet]
python3 -I tools/bugreport.py diff-hashes A B
```

- `info` prints the summary a reader starts from: first each knob changed
  live after boot (`knob changed live: sector_visibility = game (changed
  live at 85.5 s, frame 2557; boot frustum, default frustum; render)`), the
  knobs whose value differs from the declared default, or that the bundle
  has no `knobs.txt`; then build, exe SHA-256,
  game, disc, stamps, mods, press and anchor counters, the state summary,
  taint counts, members and the hash check, redaction findings, a pace
  summary over the last 30 s of present records, the last `--log-lines`
  (default 20) WARN and ERROR log lines, and the replay route with its
  command.
- `extract` writes every member under DIR and verifies each against the
  manifest.
- `replay` runs the route `info` names and prints the run's
  `bug-report-replay: knob` lines and its verdict line. The run's output goes to
  `<work>/replay.log`; `--work` defaults to `bugreport-replay/<bundle stem>`
  under the working directory. `--disc` defaults to
  `games/<game>/<game>/build/<manifest disc>`. `--timeout` defaults to
  1800 s. `--dry-run` prints the route and the command only. Arguments
  after `--` go to the exe.
- `subtick` reads `subtick.bin` (BUNDLE may be the extracted member):
  per object class the records drawn over the held presents and how many
  were drawn on intermediate presents; with `--class` or `--key`, each
  selected record's last `--last` draws (present, kind, path, alpha,
  verdict, keyframe verdict, the pair's phase, screen centroid, drawn
  world position, the position the pair lerps to). It flags MULTI, a
  record drawn at screen positions more than `--tol-px` (default 2) apart
  within one present, and OFFLERP, a paired record drawn more than
  `--tol-world` (default 2) world units from both its lerp and its curr
  position; exit 2 when either flag fires. `info` prints the ring's
  one-line summary.
- `audio` reads `audio-feed.txt` (BUNDLE may be the extracted member):
  the header and its notes, the counter lines, and the events with the
  ratio in cents; `--kind` lists the named events only, `--no-ticks`
  leaves out the samples, `--last` keeps the last N. Exit 3 (VACUOUS)
  when the feed played data and the ring holds no event. `info` prints a
  one-line summary: mute, device, events by kind, underruns, primes,
  stretch engages, producer speed and pitch p99.
- `perf` reads the performance records (BUNDLE may be an extracted
  `pace.oppl`): the manifest's `records` field; over the pace ring the
  present rate over the window and over its last 10 s, the present
  interval p50, p99 and maximum, the sim tick p50 and p90 (the drawing
  VBL's fiber run: TICK_SPLIT's, else the longer run of each VBL pair),
  scenes and sheds per canonical present; the TICK_SPLIT p50 per slot;
  the EAGER_PLAN means of slots, afford and planned scenes, level p50 and
  maximum, the share of cut spans and the budget p50; the GPU_FRAME p50
  and p99; and the coarse rows' per-row present rate p10, p50 and p90 with
  the first and last row's. `--rows` prints the coarse rows as TSV. `info`
  prints a one-line count of the records.
- `corpus` takes directories (every `bug-*.zip` inside) and bundle paths
  and writes one TSV row per bundle: name, error, build, commit, game,
  level, trigger, platform, GPU, driver, refresh, present mode,
  fullscreen, `--render-fps`, eager, swapchain extent, enabled mods, the
  note's first 80 characters, the records it holds, the `perf`
  rollups, and the audio feed's device, mute, event count, underruns,
  stretch engages, producer speed and pitch p99. A bundle without a record kind leaves its columns empty; a
  bundle that does not open gets a row with the reason. The summary
  counts bundles per record kind, gives the median rollups per game and
  GPU, and lists the five bundles whose last 10 s present rate is
  furthest below their target (`--render-fps` when below the refresh,
  else the refresh). Bundles stay local: a received bundle holds the
  tester's savestates, so the corpus is a directory outside the
  repository.
- `diff-hashes` names the first divergent tick and the regions that
  differ between two region-hash CSVs; A or B may be a bundle, whose
  `hashes.csv` is read.

| Exit | `bugreport.py` |
| --- | --- |
| 0 | verified; IDENTICAL |
| 1 | a member fails its manifest hash or size |
| 2 | DIVERGED |
| 3 | VACUOUS: no rows compared, no verdict line, or the replay timed out |
| 4 | REPRODUCED: the replay of a fatal bundle exited fatally as the tester's run did |
| 64 | refused: malformed bundle, bad arguments, no anchor, disc not found, replay refused |

## Replay routes

`bugreport.py replay` chooses the route.

**Primary**: the release archive holds the artifact the tester ran. The
reader looks in `<archive>/<build_version>-<build_commit>/` first (only
when both fields are plain names: letters, digits, `.`, `_`, `+`, `-`,
not starting with `.`), then
every directory of the archive, for an `EXE-SHA256SUMS` line whose hash
equals the manifest's `exe_sha256` (the executable inside the artifact,
which is what the writer hashes); `info` names the artifact, its kind and
the exe path inside it. `replay` unpacks the artifact into
`<work>/artifact/` and refuses it (exit 64) when the unpacked exe's
SHA-256 is not the manifest's: a tarball is untarred and its exe runs; an
AppImage is extracted with its own `--appimage-extract` (no FUSE) and
`squashfs-root/AppRun` runs; a Windows zip is unzipped and the command is
printed, not run (exit 64), since the Windows exe is driven on the Windows
host. The archive is `--archive`, else `OPENPETE_RELEASE_ARCHIVE_DIR`, else
`${XDG_DATA_HOME:-~/.local/share}/openpete/release-archive`.

**Fallback**: no archived artifact matches, or `--exe` was given. The
command runs `--bug-report-replay` on `--exe`, default
`build/linux-release/openpete`. The route line says whether that exe's
SHA-256 equals the tester's.

Either route runs the exe with `--game <game> --headless --input-lock
--vibration 0 --mute 1 --disc <cue> --bug-report-replay <bundle>`, the
bundle's card images extracted to `<work>/cards/` as `--card1`/`--card2`
(no card flag for a port the bundle holds no image for: the replay passes
`--no-cardN`, or, when the anchor held a card with no image, a card the
game had not read where no file existed, a fresh card path in its scratch
directory, provisioned on first access), `OPENPETE_VIBRATION=0`, and
`OPENPETE_MODULE_MARCH` set to the manifest's `module_march`.

## `--bug-report-replay PATH`

A debug flag (absent from `--help` in the `*-launcher` builds). PATH is a
zip or an extracted directory. `--disc` is required: the tester's disc
path is not in the bundle, and a replay without it is refused with
`REFUSED pass --disc ...` naming the manifest's disc and SHA-256.

1. Every member is verified against `manifest.json` and copied to the
   scratch directory, `<bundle without .zip>.replay/`
   (`OPENPETE_BUGREPORT_REPLAY_DIR` names another). Before anything is
   read, the sizes the zip directory (or the directory bundle's files)
   declare are checked against the reader's bounds: 1 GiB per member,
   8 GiB for the listed members together, 16 MiB for `manifest.json`,
   100000 zip entries. A zip holding two entries of one name (compared
   ignoring case), a name listed twice in the manifest, and a directory
   bundle member that is a symbolic link or lies under one are refused,
   as is a member under `sandbox/` or named as the scratch marker, the
   paths the replay itself owns in the scratch directory.
   Emptying a scratch directory from an earlier replay removes a link in
   it without following it.
2. The command line is rebuilt: the tester's argv without its output,
   route and path flags and without every flag that only modifies an
   output (`--screenshot-native-only`, `--screenshot-overlays`,
   `--screenshot-freeze-backdrop`, `--screenshot-presents-start`,
   `--screenshot-presents-count`, `--record-cfr`, `--record-fps-overlay`,
   `--no-record-audio` and the other `--record-*` settings; the list is
   `k_drop` in `core/record/bugreport_replay.c`), `~` remapped into the
   scratch directory; the developer's own capture flags decide what the
   replay writes. Display flags (`--renderer`, `--window`, `--fullscreen`,
   `--vsync`, `--gpu`, `--ssaa`, `--aspect`) pass. Then the
   replay's flags (headless, the scratch cards, a fresh scratch card path
   for a port whose anchor held a card with no image, the bundle's `mods/` as the
   mods root, `anchor.state` as the `--resume-handoff` file, `segment.bk2`
   as the `--bk2` movie at the anchor's consume point); then the
   developer's own flags. The environment takes `env.txt`'s `OPENPETE_*`
   variables except the harness triggers below, `OPENPETE_HANDOFF_AT`,
   `OPENPETE_FRAME_LIMIT`, `OPENPETE_CONFIG`, `OPENPETE_LOG_FILE` and the
   mouse injection variables; the bundle's `config-files/` become the two
   config tiers (`--no-config` when it holds none). Every other
   `OPENPETE_*` variable of the developer's environment is unset, except
   the `OPENPETE_BUGREPORT_REPLAY_*` ones and those
   `OPENPETE_BUGREPORT_REPLAY_ENV` names (below): the tester's session is
   replayed on the tester's environment, and an instrument variable of the
   developer's reaches it only when named there. One line lists the
   variables unset:
   `bug-report-replay: env not forwarded from this shell (N): A B ...; ...
   name an instrument variable in OPENPETE_BUGREPORT_REPLAY_ENV to forward it`.
   The replay adds `--input-lock 1`: no host input reaches the game. A
   headless replay without a screenshot request brings up no native
   renderer (`--renderer vk ignored under --headless`), so a native-path
   census reads nothing unless `OPENPETE_HEADLESS_NATIVE=1` is forwarded
   (`OPENPETE_BUGREPORT_REPLAY_ENV=OPENPETE_HEADLESS_NATIVE`) or a
   screenshot is requested.
   The launch scripts, `--skip-to-level`, `--skip-to-area`,
   `--skip-to-cutscene`, `--player-pose` (and `OPENPETE_PLAYER_POSE` in
   `env.txt`) and `--cheats`, write guest RAM once at a point of the
   session they wait for and keep their progress in host state the anchor
   does not hold. They pass only when the anchor is the session's first
   (manifest `anchor.seq` 1): it is taken at the first admissible yield
   after fast boot, before any of them runs, and a guest state load drops
   every anchor. From a later anchor they are dropped. A manifest without
   `anchor.seq` is decided by `anchor.frame` 1 (label `launch-inferred`).
   The `bug-report-replay: launch flags kept|dropped: <reason>: <flags>`
   line names the decision. `--load-delay` acts on every tick and always
   passes.
3. When this exe's SHA-256 equals the manifest's, the bundle's
   `module/<file>` is passed as `--recomp-module` (label `primary-route`):
   the tester's module, compiled for the tester's CPU, is loaded as is.
4. Admission: the game must match; the ROM stamps must match (when either
   side has none, the disc SHA-256 must); the anchor's state identity must
   recompute from the manifest's codegen and ROM stamps under this build's
   state ABI. Labels: `cross-disc` (ROM stamps equal, disc SHA-256
   differs), `cross-module` (codegen stamp differs), `cross-build` (build
   version differs), `cross-regions` (the region tables do not share every
   name; totals are then not compared).
5. The replay writes nothing into the user's directories. After
   `env.txt` it makes `<scratch>/sandbox` the process's sandbox root
   (`OPENPETE_SANDBOX`, [unattended runs](unattended-runs.md#the-sandbox)):
   the log file and the hang watchdog's files go to
   `<scratch>/sandbox/state/openpete/logs/`, a bundle the replay writes
   (its own fatal exit, a key press) to
   `<scratch>/sandbox/data/openpete/bugreports/`, and every other per-user
   file under the same root. A tester's `OPENPETE_SANDBOX`,
   `OPENPETE_LOG_FILE`, `OPENPETE_HANG_TRACE` and `OPENPETE_HANG_RAM` are
   never used; `OPENPETE_BUGREPORT_REPLAY_ENV` may still name the two hang
   variables, and the developer's `--log-file` wins. The
   `bug-report-replay: outputs under` line names the root.
6. The anchor loads at its park site on an empty CD queue, waiting up to
   9000 park waits (`OPENPETE_BUGREPORT_REPLAY_SITE_WAIT`), with the movie
   held until the load commits and the anchor's card images restored at
   the load. Each tick's region-hash row is compared with `hashes.csv`; at
   the press frame the state is captured and compared with `press.state`
   section by section. A fatal-exit bundle (manifest `press` null, trigger
   `hang` or `cdsync-spin`) holds no `press.state`: it replays to the last
   row of `hashes.csv`, compares hash rows only, and the verdict line
   carries `sections=skipped fatal=<trigger>`. It then runs one more tick,
   the one the tester's run did not complete.
7. `--bug-report-replay-beyond TICKS` (debug flag, default 0) runs TICKS
   ticks past the end frame (the press frame, or a fatal bundle's last
   row; a fatal bundle runs at least one), with no hash row or section
   compared there, and prints `bug-report-replay: beyond the recorded
   window: ticks A..B run on neutral input`. Input there is neutral: the
   movie ends at the press, so the pad holds no button from the first row
   past the segment (the bk2 log line `release=[...]` at that row), and
   the input lock keeps host input out. Neutral rather than the last row
   held: the tester's input past the last row is not recorded (a hung
   game reads none), a held row would invent a hold (a charge, a run
   into a wall) the tester may never have made, and neutral needs no
   input path beyond the movie's own end. It is the
   way to see what a build does after the tick where a hang bundle's
   build hung: on a build without the hang (or with the watchdog
   threshold forwarded higher), add `--screenshot-on-frame` or a census
   at a tick inside the window. A replay with `--screenshot-on-frame` and
   the `-og` leg on runs at least one tick past the end frame: the `-og`
   file pairs with the PsyCross present that rasterized its scene's OT,
   one tick after the key when the game submits after VSync, and a run
   stopped at the end frame leaves the provisional image of the tick
   before (`rule=provisional` on the `screenshot: wrote` line).
8. A fatal exit of the replay itself (the hang watchdog, the CdSync spin
   guard) prints the verdict from the exit flush, before its own bundle is
   written, and exits with the verdict's status. The line carries
   `replay-fatal=<reason> tick=<T>`, the tick that did not complete
   (the frame counter plus one), and for a hang `hang-at=0x<addr>
   hang-kind=entries|loop|stall`: the guest function whose entry crossed
   the entry threshold, the header of the call-free loop whose quanta
   crossed theirs, or the entry or header that took the stall monitor's
   request (0 when it dumped off-thread). The next line names the scratch
   directory holding the hang files and the bundle.

The verdict line on stderr:

```
bug-report-replay: IDENTICAL rows=N sections=equal [beyond=K/B] [replay-fatal=...] labels=L taint=T [vram-differs=...]
bug-report-replay: IDENTICAL rows=N sections=skipped fatal=hang recurred=no beyond=K/B [replay-fatal=...] labels=L taint=T
bug-report-replay: REPRODUCED rows=N matched=N fatal=hang replay-fatal=hang tick=T hang-at=0xA hang-kind=K labels=L taint=T
bug-report-replay: DIVERGED tick=F region=R|fatal section=S rows=N matched=M [fatal=...] [beyond=...] [replay-fatal=...] labels=L taint=T
bug-report-replay: VACUOUS rows=0 matched=0 (the run ended at frame F, press frame P) [replay-fatal=...] labels=L taint=T
bug-report-replay: REFUSED <reason> labels=L
```

| Exit | `--bug-report-replay` |
| --- | --- |
| 0 | IDENTICAL |
| 2 | DIVERGED or VACUOUS |
| 4 | REPRODUCED |
| 64 | refused |

The verdict word is decided on the recorded window: the hash rows, the
press sections, and for a fatal bundle the tick after its last row.
`matched` counts the rows compared equal before the first difference.
REPRODUCED is a fatal bundle replayed to a successful reproduction: every
row matched and the replay's own fatal exit has the bundle's trigger at
the tick after the last row, the tick the tester's run did not complete.
`recurred=no` on a fatal bundle's IDENTICAL says that tick completed: on
the build that failed, the fatal exit is not reproducible from the anchor
(a stall-monitor trip measures wall time); on a build with a fix, it is
the fix's evidence. A fatal exit of the replay inside the window that is
not the bundle's (another trigger, another tick, or any in a press
bundle's window) is DIVERGED with `region=fatal` and its tick. `beyond=K/B`
counts the ticks run past the end frame of the B asked; a fatal exit
there is printed on the line and decides neither the word nor the exit
status.

A run that ends before the press frame (for a fatal bundle, `last hash
row`) after comparing equal rows reports DIVERGED in the VACUOUS form,
`section=not-reached` when a row differed first. A DIVERGED line is followed by a
`sections differing:` line naming every press-state section that differs. VRAM is not
gameplay state: a VRAM difference is printed (`vram-differs=` on
IDENTICAL, a `vram differs (not counted)` line on DIVERGED) and does not
make the verdict DIVERGED. `taint` counts the events since the anchor that
a replay cannot reproduce.

`tests/harness/check_bugreport_replay.sh` (ctest `bugreport_replay`)
records two Spyro 2 bundles headless under `OPENPETE_BUGREPORT_AT`, one
with `--skip-to-level`, and requires IDENTICAL replays of both, DIVERGED
with the launch flags dropped when the skip bundle's `anchor.seq` is set to
2, an `OPENPETE_BUGREPORT_REPLAY_ENV` instrument's output under the replay,
and the refusal of `OPENPETE_PLAYER_POSE` there. Its knobs case records a
bundle with three renderer knobs set live and not saved (two before the
anchor, one after it, under `OPENPETE_BUGREPORT_KNOB_AT`) and requires the
replay to apply all three, a native screenshot at feed key 4390 equal to
the recording's, the same screenshot to differ under
`OPENPETE_BUGREPORT_REPLAY_KNOBS=0`, zero applied knobs to read VACUOUS,
and the bundle without `knobs.txt` to replay IDENTICAL. Its hang case
records a fatal bundle under `OPENPETE_HANG_THRESHOLD=2000` (the entry
watchdog trips as Summer Forest goes live) and requires the replay on
the same build to exit 4 REPRODUCED at the bundle's last row plus one,
with its log, hang files and bundle in the scratch directory and none in
the directories standing for the user's; the same bundle with the
threshold forwarded at 50000000 and `--bug-report-replay-beyond 300` to
replay IDENTICAL `recurred=no beyond=300/300`; the skip bundle with the
low threshold forwarded to be DIVERGED `region=fatal`; a bundle pressed
while a corpus movie holds square to release it at the first row past
the segment under `--bug-report-replay-beyond 60`; the `env not
forwarded` line; and the refusal without `--disc`.

### Display path of a headless replay

A replay runs `--headless` unless `OPENPETE_BUGREPORT_REPLAY_WINDOWED` is
set. Under `--headless` the native renderer comes up on a surfaceless
Vulkan device only when the run has a consumer for it
(`openpete_native_headless_consumer`): a `--screenshot-on-frame` or
`--screenshot-intermediate-on-frame` request, the identity audit
(`OPENPETE_IDENT_AUDIT`), or `OPENPETE_HEADLESS_NATIVE=1`. Without one the
native renderer stays off and PsyCross keeps its GL path. PsyCross skips
its own GPU work only in a headless run with `--screenshot-native-only`, no
`--record`, and a native consumer; both decisions ask the same predicate,
so a run always has one display path. An instrument that reads the native
path in a replay therefore needs one of the consumers: a screenshot flag
after `--` (`bugreport.py replay`) or on the command line, or
`OPENPETE_BUGREPORT_REPLAY_ENV=OPENPETE_HEADLESS_NATIVE=1`. A windowed
replay reaches the live present, which a headless run never builds:
occlusion readback and eager production differ there.

## A tester's bundle is untrusted input

A bundle comes from another machine and is read as untrusted input.

- `tools/bugreport.py` reads members by name with size bounds; refuses
  absolute names, `..` components, drive letters, NUL bytes, duplicate
  names and symlinks; never executes or imports anything from a bundle; and
  runs under `python3 -I` with the standard library only. `info`,
  `extract`, `subtick`, `audio`, `perf`, `corpus` and `diff-hashes` run
  nothing from the bundle. `replay` unpacks an archived artifact with
  unsafe names, links and devices skipped.
- `--bug-report-replay` refuses a member name that is empty, absolute
  (`/` or `\`), holds a `:` or a `..` component, and a member whose SHA-256
  differs from `manifest.json`. The manifest is itself a member, so the
  hashes prove the bundle is internally consistent, not where it came
  from.
- A replay executes code from the bundle. It passes the bundle's `mods/`
  as `--mods-dir`, and the mod loader compiles each enabled mod's
  `src/*.c` and loads the resulting shared object, or loads a prebuilt
  object as is (`core/mods/mod_loader.c`). On the primary route it loads
  the bundle's `module/<file>` as `--recomp-module`. The bundle's
  `config-files/` become the config tiers and `env.txt`'s `OPENPETE_*`
  variables are set.
- Before replaying a bundle from outside the project, read it with
  `bugreport.py info --members` and review `mods.txt` and every enabled
  mod's files under `mods/<id>/`. A replay with `-- --mods ""` (developer
  flags come after the replay's, and `--mods` beats `OPENPETE_MODS`)
  enables no mod; a mod that writes guest RAM then makes the replay
  DIVERGED, which is the expected verdict for a session the mods shaped.
- Replaying a bundle runs its sender's code and settings with the
  developer's privileges, and `--mods ""` does not prevent it. When the
  manifest's `exe_sha256` equals this executable's hash, the replay
  loads `module/<file>` as `--recomp-module`; the hash comes from the
  bundle, so a bundle can state the hash of any published build. The
  bundle's `config-files/` are applied as the config tiers without a
  filter: an `[env]` table sets any environment variable, `OPENPETE_*`
  or not, and an `[args]` table adds any flag, including ones that name
  an output file. Every `OPENPETE_*` line of `env.txt` that the replay
  keeps is set, including the compiler a mod build runs
  (`OPENPETE_MOD_CC`). A mod shipped as a prebuilt object is loaded as
  is.
- Replay a bundle from outside the project only on a disposable machine
  or user account that cannot read the developer's home directory, or
  after `bugreport.py info --members` shows no `module/` member, no mod
  object, and no `[env]` or `[args]` table in `config-files/`, and
  `env.txt` has been read.
- Bundles stay outside the repository: a bundle holds the tester's
  savestates, card images, log tails and configuration. Redaction covers
  the home directory, user name and host name in text members only;
  `redaction.flagged` names each other member that holds one.

## Release archive

`scripts/package-dist.sh` (outside a container) and
`scripts/release-container.sh` (on the host, after the container exits)
copy every package they produce, tagged or not, into

```
${XDG_DATA_HOME:-~/.local/share}/openpete/release-archive/<version>-<commit9>/
    <artifact>...
    SHA256SUMS        "<hex>  <artifact>": each artifact's own SHA-256
    EXE-SHA256SUMS    "<hex>  <artifact>:<path inside>": the executable inside it
```

`OPENPETE_RELEASE_ARCHIVE_DIR` names another root, for the scripts and
for `bugreport.py`. The writer records the SHA-256 of the running binary
(`/proc/self/exe`), which inside an AppImage is the inner binary, so
`exe_sha256` is the `EXE-SHA256SUMS` value and `bugreport.py` matches on
that file alone; an artifact with no `EXE-SHA256SUMS` line is not found.

## Harness variables

| Variable | Effect |
| --- | --- |
| `OPENPETE_BUGREPORT_AT=N` | press at feed key N |
| `OPENPETE_BUGREPORT_KNOB_AT=K:id=value,...` | at feed key K, set knob registry row `id` to `value` through its setter (an enum row's value name, `on`/`off`, or a number), as a dev overlay edit would; logged as `bugreport: harness knob` |
| `OPENPETE_BUGREPORT_REPLAY_KNOBS=0` | the replay applies no live knob value from `knobs.txt` and prints each it would have applied |
| `OPENPETE_BUGREPORT_NOTE` | `0`: never ask for the note; `1`: ask on an `OPENPETE_BUGREPORT_AT` press too. Unset: ask on a key press only |
| `OPENPETE_BUGREC_ANCHOR_AT=K1,K2,...` | take an extra anchor at each feed key, beside the 30 s cadence |
| `OPENPETE_BUGREC_DUMP_AT=N`, `OPENPETE_BUGREC_DUMP_DIR=D` | at feed key N, write the recorder's contents to D: `anchor-<i>.state`, `.card<N>.mcr`, `.modsave`, `.seg.bk2` and `.txt` per anchor, `hashes.csv`, `hosttrace.bin`, `pace.bin`, `input-events.csv`, `devices.txt`, `recorder.txt`. The first without the second logs an error and dumps nothing |
| `OPENPETE_BUGREPORT_THUMBS=0` | turn the thumbnail ring off (default `1`): 20 thumbnails in a 320x180 box, one per 250 ms of present clock |
| `OPENPETE_BUGREPORT_SUBTICK=0` | turn the sub-tick present ring off (default `1`); the bundle then holds no `subtick.bin` |
| `OPENPETE_BUGREPORT_GPU=1` | arm the GPU timestamp zones and record a GPU_FRAME per rendered frame (default `0`); read at device creation. `OPENPETE_GPU_TIMERS=1` arms the same zones and also prints the `[gpu-pass]` line |
| `OPENPETE_PACE_RING=0` | no pace ring and no coarse ring; the bundle then holds neither `pace.oppl` nor `pace-coarse.bin`, and no TICK_SPLIT, EAGER_PLAN or GPU_FRAME record is made unless `--pace-log` is open |
| `OPENPETE_BUGREPORT_REPLAY_DIR`, `OPENPETE_BUGREPORT_REPLAY_SITE_WAIT`, `OPENPETE_BUGREPORT_REPLAY_WINDOWED` | the replay's scratch directory, park-wait cap (above), and a set value runs the replay in a window instead of headless |
| `OPENPETE_BUGREPORT_REPLAY_ENV=A,B=V,...` | instrument variables the replay keeps: `NAME` keeps the developer's value, `NAME=VALUE` sets one; set after `env.txt`, each printed as `env kept`. Refused (exit 64): a name outside `OPENPETE_*`, a harness trigger the replay drops, an unset name, and a variable whose consumer writes guest RAM, feeds the pad or the CD timing, or loads mods (`OPENPETE_PLAYER_POSE`, `OPENPETE_FREECAM*`, `OPENPETE_CAMLOCK*`, `OPENPETE_CUSTOM_WARP`, `OPENPETE_CUSTOM_MODE`, `OPENPETE_SKIP_*`, `OPENPETE_LOAD_DELAY`, `OPENPETE_INPUT_*`, `OPENPETE_SYNTH_PADS`, `OPENPETE_MOUSE*`, `OPENPETE_DEVUI_INJECT_MOUSE`, `OPENPETE_REWIND_*`, `OPENPETE_MODS`, `OPENPETE_MODS_DIR`, `OPENPETE_MOD_CHANNEL_MASK`, `OPENPETE_MOD_HOT_RELOAD`, the `*_POKE` fault injectors, `OPENPETE_MODSAVE_FAULT_*`). A kept variable not on that list is unverified as neutral; the row and press-state compares report it if it is not |

A replay drops `OPENPETE_BUGREPORT_AT`, `OPENPETE_BUGREPORT_KNOB_AT`,
`OPENPETE_BUGREC_ANCHOR_AT`, `OPENPETE_BUGREC_DUMP_AT` and
`OPENPETE_BUGREC_DUMP_DIR` from the tester's environment.

Feed keys count yields on Spyro the Dragon and VBLs on Spyro 2 and 3, as
`--frame-limit` does ([Unattended runs](unattended-runs.md)).
