# Tile census

The tile census records, for every tile the presentation golden captures,
integer counters of what the composed scene held: triangles per render
path, tag family and kind, scene category, blend mode, triangle flag and
class bit, triangles per moby class, and the retained terrain-cache sectors
the scene draws. The counters are written to a sidecar file beside the tile.
`scripts/golden-fleet.py` keeps the sidecars of full runs in a census
store, and `scripts/golden-census.py` answers from that store which tiles
a change can move.

Sources: `core/diag/tile_census.def` (every counter name and what it
counts), `core/diag/tile_census.c` and `core/include/openpete_tile_census.h`
(tally, names, sidecar writer), the capture helpers `census_shot_record` and
`census_shot_write` in `core/renderer/native_render.c`, the class bits
`OPENPETE_CB_*` in `core/include/openpete_native_render.h`.

## Enabling it

`OPENPETE_TILE_CENSUS` set to any value other than empty or `0` makes each
native capture written to files record its tally and write a sidecar. The
variable is read once, at the first call of `tile_census_on()`
(boot-latched; registry row `TILE_CENSUS`, Debug diagnostics). When the
value is a 40-digit lower-case hex tree hash, the sidecar header names it;
any other value writes `tree unrecorded`.

Counting selects no code path: the tally reads the scene the compose draws
in every build, and the variable decides only whether the tally is kept and
written. `scripts/fences/tile_census_accessor.py` enforces this: the
variable is read only in `core/diag/tile_census.c`, and `tile_census_on()`
is called only from `census_shot_record` and `census_shot_write`. The fence
runs in the `pre-commit` hook (`hook`) and as `tree` over every source;
exit 66 names a refused call or read, 67 a missing helper.

The presentation tier of `tests/harness/check_presentation_golden.sh` sets
the variable on every capture leg (`pg_leg_env` in
`scripts/presentation-game.sh`), to the `source-tree=` value of the
candidate's `capture-record.txt` when it has one, else `1`. The dragons
tier writes no sidecar. A leg whose PNG exists without its sidecar fails
with `MISSING-CENSUS <sidecar>` and the binary's `tile-census` `FAIL` lines
(`pg_census_check`); a leg's sidecars are deleted before it runs.

## The sidecar

A capture whose path base is `<base>` writes `<base>-vk.png` and, with the
census on, `<base>-vk.census`:

```
# gen <G> tree <T>
<name>	<value>
...
```

`G` is the registry generation `TC_GEN` of the writing build; `T` is the tree hash or `unrecorded`. Then one
`name<TAB>value` line per nonzero counter, sorted by name in byte order. A
counter not listed is zero. The file is written through a temporary file
and a rename.

The sidecar holds:

- the tally of the scene the capture composes (no prefix);
- `compose.vram` = 1 when the compose found no content and shows the PSX
  VRAM framebuffer (a load screen); the scene counters then describe
  triangles the compose did not draw;
- the tally of the frozen backdrop the tile draws, every tally, bit and
  cache counter under the prefix `backdrop.`, taken when the backdrop was
  rendered by the headless settle; or `backdrop.unknown` = 1 when the tile
  draws a backdrop whose tally is not kept.

A sidecar is refused (not written; the log says
`FAIL <path>: <reason>; no sidecar written`) when:

| Reason | Cause |
|---|---|
| `counter table full` | more than 2048 counter names, or a name of 64 characters or more |
| `no render-path mapping registered` | the game layer did not call `openpete_native_register_tag_paths` |
| `more moby classes than the tally holds` | more than 512 distinct moby classes in one scene |
| `a class bit with no registered counter` | a triangle carries an `OPENPETE_CB_*` bit with no `TC_BIT` row |
| `binding: ...` | the tally does not describe what was drawn: a VRAM-fallback compose composed triangles, the compose drew the scene other than once, or a category's tallied count differs from the composed count |

The binding check compares the tally recorded inside the last compose
attempt with the per-category triangle counts `draw_native_scene` submitted
while the record was armed.

## Counters

Names are patterns over three placeholders: `<path>` is a render-path name
(`cyclorama`, `spyro`, `moby`, `terrain`, `shadow`, `flame`, `particles`,
`tracer`, `transition`, `portal`, `glow`, `sparkle`, `moby_shadow`, `ui`,
in `render_path_t` order of `games/common/renderer/render_paths.c`);
`<family>` is a tag-family name (`moby`, `terrain`, `spyro`, `flame`,
`particles`, `cyclorama`, `shadow`, `tracer`, `portal`: tag bits 31:28 = 1
to 9); `<n>` is a decimal integer.

| Name | Unit | Counts |
|---|---|---|
| `<path>.tris` | tris | triangles whose provenance tag maps to the path (`tag_kind_path`). The tag is sticky: the level-transition dome and the native GTE feed set none, so their triangles count under the family set last. Untagged 2D, HUD and text triangles count under no path. Retained cache sectors add no triangle. |
| `<family>.tris.k<n>` | tris | triangles of the family and tag kind `n`: the particle family's feeder kind (k2 glow rings, k3 sparkle flares, k4 sparkle lines, k5 the dragon-rescue star), the shadow family's caster kind (k0 moby, k1 player), k0 for every other family; kinds past 7 fold into k7 |
| `tag.unmapped.tris` | tris | triangles whose tag family (10 to 15) maps to no render path |
| `cat.<n>.tris` | tris | triangles of scene category `n` (`OPENPETE_SCENE_*`, 0 WORLD to 14 TERRAIN_KEY; 15 holds any other value) |
| `blend.<n>.tris` | tris | triangles of blend mode `n` (0 opaque, 1 alpha, 2 additive, 3 subtract, 4 quarter-add; 7 holds any value outside 0 to 6) |
| `flag.shell.tris` | tris | front-shell record members (`scene_tri_t.shell`) |
| `flag.key_order.tris` | tris | triangles that draw by key order against terrain |
| `flag.kfold.tris` | tris | triangles whose drain quadrant folds |
| `flag.portal_rim.tris` | tris | opaque terrain triangles accepted as a portal's inner rim |
| `flag.host_added.tris` | tris | triangles the host added beyond the game's own render |
| `flag.fog_delta.tris` | tris | triangles drawn from a shifted fog CLUT row |
| `flag.over_fade.prims` | prims | 2D primitives the game links after the fade quad: runs of consecutive flagged triangles sharing one primitive address; a flagged triangle with address 0 counts alone |
| `flag.under_player.prims` | prims | flat 2D primitives drawn before the player, counted the same way |
| `flag.carry.prims` | prims | 2D-UI sub-tick carry primitives, counted the same way |
| `moby.type.<n>` | tris | moby-family triangles of moby class `n` (tag bits 15:0) |
| `terrain.sec.retained` | sectors | retained terrain-cache sectors the scene's state draws (`OPTC_SF_DRAWN`); zero unless the cache runs, which on a software device needs `OPENPETE_TERRAIN_CACHE_FORCE=1` |
| `terrain.sec.retained_hp`, `_lp`, `_near` | sectors | retained sectors drawn at the high-poly tier, the low-poly tier, through the near variant |
| `compose.vram`, `backdrop.unknown` | flag | above |

### Class bits

Each scene triangle carries a word of class bits, `openpete_scene_tri.cbits`,
set where it is emitted (`games/common/renderer/moby.c`,
`games/common/renderer/terrain.c`). The census counts the triangles that
carry each bit. A bit is never reused; a retired bit keeps its row.

| Bit | Define | Counter | Set on |
|---|---|---|---|
| 0 | `OPENPETE_CB_MOBY_GPF` | `moby.gpf.tris` | moby triangles of a record whose GPF translation rescale ran: the record flags word's low byte nonzero on a game whose cull stores the unscaled translation (the scale override) |
| 1 | `OPENPETE_CB_MOBY_ENV` | `moby.env.tris` | moby triangles of a face that takes environment-map UVs: Spyro 1's metal reflection faces, Spyro 2 and 3's mode-1 special faces whose substitute page and generated coordinates apply |
| 2 | `OPENPETE_CB_MOBY_SIMPLE` | `moby.simple.tris` | moby triangles of a simple-model record ([Glossary](glossary.md)) emitted through the `moby_simple_` calls |
| 3 | `OPENPETE_CB_MOBY_SCREEN` | `moby.screen.tris` | simple-model triangles projected at a named screen origin (screen space) |
| 4 | `OPENPETE_CB_MOBY_UNR` | `moby.unr.tris` | moby triangles with a vertex in the GTE divide-overflow regime (the hardware-clamped screen pair) |
| 5 | `OPENPETE_CB_MOBY_SAT` | `moby.sat.tris` | moby triangles with a coordinate-saturated vertex |
| 6 | `OPENPETE_CB_MOBY_NEARCUT` | `moby.nearcut.tris` | the parts of a walk triangle the moby near clip cut; a triangle wholly behind the plane is dropped and counts nowhere |
| 7 | `OPENPETE_CB_MOBY_TIEPULL` | `moby.tiepull.tris` | opaque keyed actor triangles whose cross-moby tie pull is nonzero (a face coinciding with an earlier record's face of its world bucket); not the within-record coincident pull |
| 8 | `OPENPETE_CB_TERRAIN_NEARCLIP` | `terrain.nearclip.tris` | the parts of a walked terrain triangle the near plane cut; retained cache sectors clip on the GPU and count nowhere |

Bit 2 is set by `moby_simple_record` and `moby_simple_project_record`,
which the Spyro 2 and 3 second-kind pass calls. Spyro 1 draws its simple
models through the shaded pass, which clears the record word, so on Spyro 1
`moby.simple.tris` and `moby.screen.tris` stay zero.

### Generations

`tile_census.def` holds one `TC_ROW` per counter or counter family:
`TC_ROW(id, name, unit, source, path family, since, state, meaning)`.
`source` is `tally` (the scene's triangles), `bit` (a class bit), `cache`
(the retained terrain state) or `capture` (the capture helpers). A row is
never deleted; a counter that stops being written is marked `retired`, so
every name in an old sidecar resolves. `TC_GEN` rises by one in every change
that adds a row or changes what a row counts, and that row's `since` is the
new generation. A sidecar written at a lower generation than a row's
`since` holds no value for it: the value is unknown, not zero, and
`golden-census.py` prints it with `?` and treats it as engaged.

Adding a class bit: define `OPENPETE_CB_<NAME>` at the next free bit, raise
`OPENPETE_CB_COUNT`, add `TC_BIT(<bit>, "<counter>")` and its `TC_ROW` with
`source` `bit` and `since` the raised `TC_GEN`. A bit without a `TC_BIT`
refuses every sidecar of a scene that carries it.

## Census store

The fleet coordinator pulls every presentation tile's sidecar with the
unit's `tile-digests.tsv` (written by the verdict through
`golden-census.py digests`); a captured tile without its sidecar refuses
the unit. An accepted presentation unit enters the store when its run
records none of `filter`, `freecam_filter`, `rows` and `repeat` in
`run-args.json`, the unit was not stopped, and every sidecar's header names
the capture record's `source-tree` (else `census store entry refused:
<reason>` is logged and no entry is made). Layout, under the local host's
`fleet_root`:

```
<fleet_root>/census/<game>-presentation/
  <run-id>.part/          an entry being written: sidecars and tile-digests.tsv (hard links), accept.json
  <run-id>/               a finalized entry:
    *-vk.census           every captured tile's sidecar
    tile-digests.tsv      `tile<TAB>digest16` per captured tile
    moved.txt             the accepted attempt's DIVERGE, EXTRA and EXCEED tiles from stream.log
    run-args.json, done   copies of the run's files
    accept.json           the coordinator's record at the pull
    entry.json            run_id, unit, attempt, head, source_tree, rc, compared, sidecars, written; written last
```

The `.part` is finalized after the run's done marker and renamed to
`<run-id>/`. Each unit keeps its two newest finalized entries; a `.part`
older than 24 h is removed. `golden-fleet.py clean` touches the store only
under `--all`.

A unit's counts source is, among its entries that have `entry.json` and
`done` and whose `run-args.json` records no filter key, the newest whose
`head` is a first-parent ancestor of HEAD, else the newest, with a note.
No reader falls back to an older entry, a `.part`, a filtered run or the
fleet's stream directories. Every answer opens with one line per unit:

```
[golden-census] spyro-2-presentation: run <run-id>, head <sha12>, tree <tree12>, done <time>; HEAD is <k> first-parent commits past it, <r> touch the renderer; <u> row(s) unknown (added since), <d> removed since
```

A manifest row with no sidecar in the entry (added since the run) is
`unknown`: every predicate of its game holds it.

## `scripts/golden-census.py`

Every reading subcommand takes `--store DIR` (default
`<local fleet_root>/census`, the `fleet_root` of the host file's local
host). Exit 0 done, 1 refused or no counts source (`[golden-census]
REFUSED: <reason>`), 2 usage.

A predicate is a conjunction of terms joined by `and`:

- `counter OP integer`, `OP` one of `>`, `>=`, `<`, `<=`, `==`, `!=`; the
  counter must resolve to a registered row. The term holds on the tile's
  own value or its `backdrop.` value, unless the predicate holds `own`.
- the leg terms `canon`, `inter`, `ident`, `lodext` (the tile is the
  extended-LOD leg), `orig` (it is not);
- `own`;
- `launch:<substring>`, matched against the row's manifest launch field.

Tile names follow the presentation golden:
`<row key>__[lodext__]<canon|inter|ident>__<n>-vk.png`, the row key being a
bk2 row's name or a freecam pose's `<level>-<pose md5 prefix>`.

### `query`

`query (--game G | --all) PRED [--rows]` prints the tiles the predicate
selects, by the value of its first counter term, highest first
(`<game> <tile> <value> <row>`), or with
`--rows` one line per row (`<game> <row> max=<v> tiles=<k>`), then the
unknown rows. With no tile selected it says a row that engages the counter
is owed.

```sh
# Spyro 2 extended-LOD tiles where the moby near clip cut a face:
scripts/golden-census.py query --game spyro-2 'moby.nearcut.tris > 0 and lodext' --rows
```

### `nearest`

`nearest --game G COUNTER [--top N]` (default 10) prints the rows with the
highest value of one counter (own plus `backdrop.`). When no tile engages
it, it says so and prints instead the rows with the highest sum over the
counter's first name component (`moby.*` for `moby.screen.tris`): the
nearest rows to extend into a row that does.

```sh
scripts/golden-census.py nearest --game spyro-3 moby.screen.tris --top 5
```

### `expect`

`expect (--game G | --all) PRED --reason TEXT` prints expected-moves lines
for `golden-fleet.py run --expect`: one `<game>-presentation <tile>
<reason>` line per selected tile and one `<game>-presentation <row>__
<reason>` line per unknown row.

`expect --query FILE [--sha SHA] [--append FILE]` runs a query file at the
commit. A query file is written by the change's author, one per commit,
tab-separated:

```
reason	<one line: which faces the change decides differently>
pred	<game,game | all>	<predicate>
hand	<game>	<tile or row substring>	missing:<counter | none>
```

A `hand` line names a tile or row the author expects to move that no
predicate selects, and the counter that would select it (`none` when no
counter could). The step: checks the file's body hash against `fixed.tsv`
in its directory (`fix`, below; an unfixed file prints `post-feedback`);
stamps the header `# commit <sha>` (default HEAD) and one
`# counts <game> <run-id> <tree12>` line per game a `pred` line names;
writes `<sha>.tiles` beside it (each `pred` line's selection with line
number, game, tile and value; its unknown rows; the `hand` lines) and fixes
that file; prints the expected-moves lines (also appended to `--append
FILE`) and the engagement line `<k> tile(s) in <r> row(s)`. A `pred` line
naming a game with no counts source refuses the step.

The body hash is the SHA-256 of the lines that do not open with `#`, so the
stamp changes no hash.

```sh
# queries/7a2640c4f.q:
#   reason	a face crossing the near plane is clipped at the near plane only
#   pred	all	terrain.nearclip.tris > 0
#   hand	spyro-2	metropolis-0c65d7	missing:none
scripts/golden-census.py fix queries/7a2640c4f.q
scripts/golden-census.py expect --query queries/7a2640c4f.q --sha 7a2640c4f --append build/expect.txt
```

### `fix`

`fix FILE...` appends `<name><TAB><body hash><TAB><UTC time>` per file to
`fixed.tsv` in the file's directory. It records the version of a query
file (or hand list) received before the batch's golden ran; `score` refuses
a file whose body hash has no `fixed.tsv` line.

### `score`

`score --run DIR [--window]` scores the query files of a full fleet run.
`DIR` is the run's local run directory, `<local fleet_root>/runs/<run-id>`;
the run must have started with `golden-fleet.py run --queries QDIR`, which
copies `QDIR` to `DIR/queries/` and records each file's SHA-256 in
`run-args.json` before any leg starts. Refused: a file whose SHA-256
differs from the record, a file added after the run started, and a `.q`,
`.q2`, `.hand`, `.hand2` or `.tiles` file whose body hash is not in
`fixed.tsv`.

Files in `queries/`: `<sha>.q` (the query file as first fixed), `<sha>.q2`
(a post-feedback version, scored apart from the first versions),
`<sha>.tiles` (from `expect --query`; required), `<sha>.hand` and
`<sha>.hand2` (an optional hand list in expected-moves form). Against the
run's `stream.log` (newest attempt per tile; DIVERGE, EXTRA and EXCEED are
moves) it prints per file the moved rows the query side holds (by `pred`,
by `hand`), recall over the moved rows, the share of streamed rows
selected, unknown rows and sole or shared attribution; then the batch's
pooled query side (`Rq`, `Sq`) and, with a hand list or `--window`, hand
side (`Rh`, `Sh`). Unknown rows are excluded from both recalls. Each moved
tile the batch did not hold is printed as `miss <game> <tile> [<class>]`
with its first counters, the class one of `environment` (the run's
`--commits` range touches no renderer path), `stale` (an unknown row, a
tile that moved in the counts source's own run, or a counter newer than the
sidecar), `leg` (its row is held, the tile is not), `excluded` (a counter
some line names is nonzero on the tile, yet no line selected it),
`uncounted` (no named counter is nonzero) or `adjudicate`. Last, per
counts source, a drift line against the unit's previous entry: tiles whose
counters changed with the digest unchanged, and tiles whose digest changed
with the counters unchanged (candidates for a missing counter).

`--window` scores a `pred` line that selects 2 rows or fewer, or carries a
`launch:` term or an upper bound (`<`, `<=`, `==`), as a hand line on both
sides: such a line can name rows its author saw move.

```sh
scripts/golden-fleet.py run --all --commits main~6..main --queries queries --expect build/expect.txt --wait
scripts/golden-census.py score --run ~/.cache/openpete-fleet/runs/<run-id> --window
```

### `sidecar-diff`

`sidecar-diff DIR [DIR2]` compares `*-vk.census` files byte for byte: two
directories by name (`ONLY` for a name in one of them), or one directory's
repeat copies (`<key>~r<k>`, from `golden-fleet.py run --repeat N`) against
the first copy of each tile. Prints `DIFFER <name>` per differing pair and
a count line; exit 1 on a difference, an unpaired name, or no pair at all
(`VACUOUS`).

```sh
# the counters of 40 repeat copies of one pose agree:
scripts/golden-census.py sidecar-diff build/fleet-stream/<run-id>/spyro-2-presentation
```

### `digests`

`digests DIR` writes `DIR/tile-digests.tsv`, each `-vk.png` tile's 16-digit
verdict digest. The presentation verdict runs it; a failure fails the
verdict.

## Tests

- `tests/unit/tile_census_test.c` (target `tile_census_test`): one counter
  per class bit under the own and `backdrop.` prefixes, a name for every
  defined bit, and a triangle carrying an unregistered bit refusing the
  sidecar.
- `scripts/fences/tests/test_golden_census.py` (ctest
  `tile_census_selftest`, labels `presentation;fence`): the store's
  finalize, prune, ancestry and refusals, query-file fixing and scoring, and
  the accessor fence's controls, on scratch copies.
