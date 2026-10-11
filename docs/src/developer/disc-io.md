# Disc I/O

Every host read of the disc image goes through one reader,
`core/psx/disc_image.c` (`core/include/openpete_disc_image.h`), addressed
by LBA over raw 2352-byte sectors. The disc cache
(`core/psx/disc_cache.c`, `core/include/openpete_disc_cache.h`) is the
reader's backing store: with it attached, a read of resident sectors is a
copy from memory and any other read is served from the file at once. The
trace (`core/diag/disc_trace.c`) records one row per host read on every
path.

## Readers

| Path | Tag | Mode | Steers the loader |
|---|---|---|---|
| PsyCross libcd (`CdRead` drains, `CdSearchFile`, the spooler) | `libcd` | cached | game slot |
| PsyCross XA pump | `xa` | cached | XA slot |
| EXE load (`SYSTEM.CNF`, the EXE, their volume descriptor and directory sectors) | `exe` | cached | no |
| Disc serial (library scan) | `serial` | cached | no |
| Recomp extract (EXE by path, WAD at LBA 37) | `extract` | cached | game slot |
| Crash demo woah NSF (`S0/S000000D.NSF`, its own thread) | `woah` (trace scope) | cached | no |
| Mod WAD overlay | `wad` | cached | game slot |
| sfx census (`--dump-sfx-pack`) | `sfx` | direct | no |
| Library ingest copy | `ingest` | direct | no |
| Mod API `read_sector` | `mod` | cached | no |
| Host text glyph model (Spyro 2 and 3 credits font, first text draw) | `font` | cached | no |

The Python offline tools (`tools/psxtrace_loads.py` and the disc-reading
scripts under `scripts/`) open the image themselves. Community mods that
read the disc through the mod API's `disc_path` open it themselves.

## Reader API

`disc_image_open(path, mode, tag)` resolves `path` (a cue or the image)
through `openpete_psx_resolve_disc_bin` (the cue-named bin, then the
same-stem `.bin`, then a lone sibling) and returns a handle. Handles are
independent: the library scan opens other images' serials while the
booted disc stays open. File reads are positioned (`pread`, or `ReadFile`
with an offset on Windows), so handles and threads share no cursor.

- `mode`: `DISC_READ_CACHED` reads through the cache when the cache holds
  the same file; `DISC_READ_DIRECT` always reads the file.
- `tag`: an `OPENPETE_DISC_TAG_*` value (`core/include/openpete_disc_trace.h`).
  The tag names the trace rows and selects the cache's steer slot.
  `OPENPETE_DISC_TAG_NONE` traces under the calling thread's scope
  (`openpete_disc_trace_scope_begin`/`_end`; the woah thread sets `woah`)
  and steers nothing; outside any scope it traces as `iso`.

Reads:

- `disc_image_read_sector(img, lba, out2352)`: one raw sector.
- `disc_image_read_user(img, lba, out2048)`: the 2048 user-data bytes,
  from byte 16 of a Mode 1 sector or byte 24 (after the subheader) of a
  Mode 2 sector, chosen by the header's mode byte.
- `disc_image_read_bytes(img, offset, dst, len, tag)`: a byte range,
  untraced; PsyCross's callback uses it. Returns `OPENPETE_DISC_HIT`,
  `_MISS` or `_DIRECT`, or -1 when a byte read as zero for want of the file.
- `disc_image_find(img, path, &entry)`: ISO 9660 (ECMA-119) lookup. The
  primary volume descriptor from sector 16, its root directory record at
  byte 156, then one directory per path component (split at `/` or `\`, up
  to 8 levels), compared case-insensitively with the `;1` version suffix
  and a trailing `.` removed.
- `disc_image_read_file(img, &entry, buf, max)`: a file's user data, read
  64 sectors per read.
- `disc_image_set_game` / `disc_image_game`: the booted disc's handle,
  for readers that hold none (PsyCross, the mod API).

`open_game_disc` in `core/runtime/run.c` opens the booted disc and, with
`--disc-cache 1`, attaches the cache before the EXE is read, so the EXE
and every later read go through the reader. `attach_disc_image` then
binds PsyCross: `openpete_disc_psycross_bind` installs a read callback
with the fork's `PsyX_CDFS_SetMemRead` and declares a bufferless image of
the handle's size with `PsyX_CDFS_Init_Mem`. PsyCross opens no file of its
own in either cache mode; its libcd and XA reads arrive at the callback
with a source (`PSYX_CDFS_READ_DATA`, `PSYX_CDFS_READ_XA`) that becomes
the `libcd` or `xa` tag. The first-boot recomp extract runs before the
cache attaches and reads the file.

### Sector size

The sector size is read off the file size at open. A whole number of
2352-byte sectors is accepted. A whole number of 2048-byte sectors is
refused with an error naming the loss: an image of user data only has lost
the Mode 2 Form 2 sectors that carry the games' XA audio; the message asks
for a raw BIN/CUE rip. Any other size is refused. On a refusal
`open_game_disc` returns `OPENPETE_EXIT_SETUP_ERROR` and the run stops.

## Disc cache

`--disc-cache 0|1` (`OPENPETE_DISC_CACHE`, default 1) attaches the cache.
`openpete_disc_cache_attach` allocates the whole image (at most
0x7FFFFFFF bytes, since `PsyX_CDFS_Init_Mem` takes an `int` size) and
starts one loader thread; without a thread the image loads inline before
the call returns.

- Residency is tracked per 2352-byte sector. A chunk is 3566 sectors
  (8 MiB rounded down to whole sectors). A chunk is missing while any of
  its sectors is, unread while any sector stayed short after 3 loader
  attempts, else resident.
- A read whose sectors are all resident is a `memcpy` (hit). A read
  touching any other sector is served from the file at once (miss): no
  read waits on the loader. A miss is counted under its tag, emits a
  pacelog `CD_MISS` record (the file read's duration and the end offset),
  and logs a warning on the first miss in its chunk. A sector the loader
  could not read whole is never served from memory.
- Steer slots: a miss sets its slot's target to the chunk of the first
  absent sector. The game slot takes `libcd`, `extract` and `wad`; the XA
  slot takes `xa`. The EXE read happens once, at VBL 0, as the cache
  attaches, and steers nothing; host reads (`woah`, `serial`, `ingest`,
  `sfx`, `mod`, `font`, untagged) steer nothing, so a host thread never
  reorders the loader ahead of the game.
- Loader order: each run is a run of missing sectors, chosen in this order:
  the game slot's chunk, the XA slot's chunk, the boot-order profile's
  ranges in profile order (a run never leaves its range), then every chunk
  in file order (a run never crosses its chunk).
- `openpete_disc_cache_shutdown` stops the loader at exit; the buffer and
  its resident sectors keep serving. The exit census line reports the
  image size, the load time, the misses per tag and the time spent reading
  the file.

## Boot-order profile

`games/<game>/manifest/disc_boot_order.toml` lists the sector ranges a
fresh boot reads, in first-use order, from VBL 1 through the last read of
the first level load (`window_end_vbl`: Spyro 1 3553, Spyro 2 3277,
Spyro 3 2459):

```toml
format = 1
game = "spyro-1"
sector_bytes = 2352
window_end_vbl = 3553
# [lba_first, lba_count, first_vbl]  first reader
ranges = [
  [37, 1, 8],  # libcd
  ...
]
```

`tools/disc_boot_order.py generate` builds it from an `OPENPETE_DISC_TRACE`
file. It keeps rows tagged `libcd`, `xa`, `exe`, `extract` or `wad` with
0 < VBL <= `--end-vbl`, ordered by VBL and then by trace order; a sector
already listed is dropped and a range that continues the previous one
merges into it. VBL 0 reads (the EXE), host-tagged rows and `cache-load`
rows contribute nothing. Only the VBL column enters, never `ns`, so one
trace gives one file byte for byte. `header` refuses a malformed profile
(wrong format number or sector size, a missing game or window, a range not
three non-negative integers, a zero count, overlapping ranges, VBLs out of
order, a VBL past `window_end_vbl`).

A game ships a profile by committing the toml and its source trace
(`tests/harness/disc_boot_order/<game>.tsv`). `cmake/openpete_game.cmake`
runs `disc_boot_order.py header` into `gen/disc_boot_order.gen.h`, and game
main calls `game_disc_boot_order_register()` (`games/common/game_build.c`),
which installs the table through the game's generated build record
(`games/common/game_build.c.in`), before `openpete_run()` attaches the
cache. A game without the toml loads in file order.

Regenerate the trace and the profile together when what the game reads
between boot and the end of its first level load changes (the disc, the
boot path, the load delay). `check` fails while the committed profile differs from
`generate` over the committed trace. Spyro 1:

```
OPENPETE_VIBRATION=0 OPENPETE_DISC_TRACE=tests/harness/disc_boot_order/spyro-1.tsv \
  build/linux-release/games/spyro-1/openpete-spyro1 --headless --no-config \
  --disc games/spyro-1/spyro-1/build/SCUS-94228.cue --renderer vk --aspect 4:3 \
  --lod original --no-card --mute 1 --frame-limit 3700
python3 tools/disc_boot_order.py generate --trace tests/harness/disc_boot_order/spyro-1.tsv \
  --game spyro-1 --end-vbl 3553 --out games/spyro-1/manifest/disc_boot_order.toml
python3 tools/disc_boot_order.py check --game spyro-1
```

Spyro 2 runs `openpete-spyro2` on `SCUS-94425.cue` to VBL 3400 with
`--end-vbl 3277`; Spyro 3 runs `openpete-spyro3` on `SCUS-94467.cue` to
VBL 2600 with `--end-vbl 2459`. `--end-vbl` is the VBL of the last `libcd`
row of the first level load's read burst in the new trace (Spyro 1 reads
it at VBL 3547-3553).

## Trace

`OPENPETE_DISC_TRACE=PATH` (an env var with no registry row: it emits a
file read offline) writes a tab-separated file with the header
`#ns vbl tag lba count result offset length`:

| Column | Content |
|---|---|
| `ns` | `op_now()` at the read, monotonic host nanoseconds |
| `vbl` | the VBL count at the read; -1 before the runtime registers its VBL source |
| `tag` | `libcd`, `xa`, `exe`, `serial`, `extract`, `woah`, `sfx`, `wad`, `ingest`, `cache-load`, `iso`, `mod`, `font` |
| `lba` | first 2352-byte sector the read touches |
| `count` | sectors the read touches |
| `result` | `hit` or `miss` (a cached read with the cache attached), `direct` (a read of the file) |
| `offset` | first byte offset in the image |
| `length` | bytes from `offset` to the end of the read |

Contiguous reads on one thread with the same tag, result and VBL merge
into one row; a read that starts in the sector after the row's last
sector also continues it. `cache-load` rows (the loader's reads, always `direct`)
never merge. A row is written when its thread's next read does not
continue it, so rows from different threads are not in `ns` order. With
`--disc-cache 0` every row is `direct` and no `cache-load` row appears.
PsyCross's reads are traced through the fork's read observer
(`PsyX_CDFS_SetReadObserver`, installed by
`openpete_disc_cache_trace_install`) with the result the reader returned.

## Harness knobs

Both are env vars with no registry row, read once at cache attach.

- `OPENPETE_DISC_CACHE_BPS=<bytes per second>` holds each loader run
  until that many bytes per second of VBL time (VBL rate 2250000/37609 Hz,
  counted from VBL 0) cover every byte charged: every byte the loader has
  read plus every whole sector a miss read from the file. A headless run
  then loads at a modelled device speed whatever the host disk.
- `OPENPETE_DISC_BOOT_ORDER=0` drops the profile; the loader runs in file
  order behind the steer slots.

## Checks

| ctest row | What it checks |
|---|---|
| `disc_image_test` | The reader on synthetic raw images: root and subdirectory lookup, case folding, `;1`, both separators, absent names; file, sector and Mode 1 user-data reads against the bytes written; the serial reader and the woah file loader; a 2048-byte and an odd-sized image refused; a cached handle hits and a direct handle reads direct with the cache attached; the booted-disc `read_sector` and its slot at the end of the mod API table. |
| `disc_cache_short_read_test` | A loader read that comes up short is retried, then served from the file, never as uninitialised heap bytes; with a profile installed the loader's first reads are the profile's ranges at sector granularity, and `OPENPETE_DISC_BOOT_ORDER=0` restores file order. |
| `exe_loader_cap_test` | The EXE loader refuses a directory size over 2 MiB + 2048 before allocating. |
| `disc_trace` | Two Spyro 1 boots to VBL 600 (cache on, cache off): every row parses; `exe`, `libcd` and `woah` rows appear and the woah NSF decodes; the `libcd` sector sum equals the CdRead thunk's; cache on has `cache-load` rows and no `direct` libcd row; cache off is all `direct`. VACUOUS when no CdRead was logged. |
| `sfx_pack` | `--dump-sfx-pack` over the Spyro 1 retail disc in direct mode: counters, the WAV name digest, every trace row `sfx` and `direct`. |
| `disc_boot_order_spyro-{1,2,3}` | `disc_boot_order.py check`: the profile equals `generate` over the committed trace, and the simulation's verdict (below). Exit 3 (VACUOUS, reported skipped) when file order misses nothing at 10 MB/s. |
| `disc_boot_order_selftest` | `tests/harness/test_disc_boot_order.py`: the parser's refusals, generator determinism (including a rewritten `ns` column), the excluded rows, and a synthetic simulation where profile order serves a read file order misses. |

`disc_trace`, `sfx_pack` and the `disc_boot_order_*` rows carry the ctest
label `disc`.

### Loader simulation

`python3 tools/disc_boot_order.py simulate --trace T --manifest M`
replays the trace's reads against the loader on the VBL clock at 10, 25,
50, 100, 200 and 500 MB/s (MB = 2^20 bytes) under file order, profile
order and profile order loaded in whole chunks. The device is shared: a
direct read is charged to the same account as the loader, as under
`OPENPETE_DISC_CACHE_BPS`. A `libcd` or `xa` row is replayed sector by
sector (PsyCross reads one sector per call); any other row is one read.
The metric is the game reads served from the file and the loader
bandwidth they take. The `check` verdict finds the lowest bandwidth at
which file order misses no game read and asserts that one step below it
profile order misses none and file order does.

Measured on the committed traces:

| Game | File order misses nothing from | One step below | File order misses there | Profile order misses |
|---|---|---|---|---|
| Spyro 1 | 100 MB/s | 50 MB/s | 4 reads, 191 sectors, 0.428 MB | 0 |
| Spyro 2 | 500 MB/s | 200 MB/s | 1 read, 1 sector, 0.002 MB | 0 |
| Spyro 3 | 200 MB/s | 100 MB/s | 2 reads, 32 sectors, 0.072 MB | 0 |

Profile order misses no game read at any modelled bandwidth; the loader
share the misses take is at most 0.65%; at 10 MB/s file order misses 10,
12 and 10 reads. Headless boots throttled with `OPENPETE_DISC_CACHE_BPS`,
under both orders, measured the misses the simulation predicts for each
game. The Spyro 1 woah NSF is a host read, never in the profile: at
50 MB/s and below it misses 2.873 MB under both orders.

## Mod API

`api->read_sector(self, lba, buf)` (api 13) reads one raw 2352-byte
sector of the booted disc through `disc_image_game_read_sector`, under the
`mod` tag: served from the cache when resident, from the file otherwise,
and never steering the loader. See
[`api->read_sector`](../modding/reference/api.md#api-read_sector) in the
Modding reference.
