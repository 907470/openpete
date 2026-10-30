# Changelog

<!--
Format, read by openpete-website/tools/gen_changelog.py, which renders the
changelog on openpete.com from this file:

  ## vX.Y.Z - YYYY-MM-DD (Status)

  - One item per bullet. A bullet continues onto following lines indented
    by two or more spaces; the lines are joined with one space.

Newest version first. The date and the "(Status)" label are each optional;
the status is shown as a pill ("Unreleased", "Current"). Items take inline
code (`x`), links ([text](url)), emphasis (*x*), and raw inline HTML where
Markdown has none (<kbd>M</kbd>, &times;). A link whose whole text is one
code span renders as <code><a>...</a></code>. A documentation page is linked
by its full https://openpete.com/docs/ URL, which resolves on GitHub and on
openpete.com; the changelog page of the shipped docs makes it relative.
-->

## Unreleased

- Mod SDK: materials of mods pinned at `api = 13` or later compose: on mobys, terrain and every other channel, the
  matching materials down to the first one pinned below 13 draw as one chain of up to four members, highest priority
  last, where the highest-priority material used to draw alone. When more than four match, the lowest-ranked member
  drops. A material sets `OP_SEL_EXCLUSIVE` (manifest key `exclusive = true`) to draw alone. A mod pinned below 13 is
  unchanged. See [Compose materials](https://openpete.com/docs/modding/shaders.html#compose-materials).
- Mod SDK: `env_anim = true` selects the terrain faces the game animates (texture swaps and scrolls, and faces whose
  vertices or colours the game moves), textured or not; it used to match every textured face. `env_anim = false`
  (`OP_SEL_ENV_STATIC`) selects the terrain faces the game does not animate, on a mod pinned at `api = 13` or later.
  See [Shaders](https://openpete.com/docs/modding/shaders.html).
- Mod SDK: the player and tracer channels carry an `ident` per draw kind: `OP_PLAYER_AVATAR`, `OP_PLAYER_MIRROR` (the
  reflection on ice) and `OP_PLAYER_SUBMERGED` (the half below the water plane) on Spyro 2 and 3; `OP_TRACER_RIBBON`,
  `OP_TRACER_MESH` and `OP_TRACER_ZAP` (Zoe's zap). A player row with `ident = 0` no longer tints the reflection or
  the submerged half. See [Shaders](https://openpete.com/docs/modding/shaders.html).
- Mod SDK: `register_shutdown_hook` installs a callback the engine runs once per mod on every deliberate exit (a quit,
  a closed window, SIGINT or SIGTERM, the end of `--frame-limit`, `--max-runtime-seconds` or `--record-frames`) and
  before a hot reload's new entry, in reverse load order and before the renderer, the texture registry and audio are
  torn down, so a mod stops and joins the threads it started. After it returns, the mod's api calls from any thread
  are refused with one logged warning. See
  [Code mods](https://openpete.com/docs/modding/code.html#threads-and-shutdown).
- Mod SDK: `texture_register` registers a mod-owned texture from code, the run-time form of
  `assets/textures/own/<hexid>.png`, legal from any context including a thread the mod starts. See
  [Textures](https://openpete.com/docs/modding/textures.html).
- Mod SDK: the Linux mod compile line defines `_GNU_SOURCE`, so a mod source can use glibc extensions such as
  `RTLD_DEFAULT` and `dladdr` from `<dlfcn.h>`; a mod that failed to compile on them now builds.
  The mod build key now covers the compile flags, so every source mod recompiles once on first boot after the update.
- Mod SDK: material priority follows the enabled list, later mods first, on every channel including terrain, where a
  refine no longer outranks a later declarative. Between two materials of one mod, a mod pinned at `api = 13` or later
  gives the later registration priority; a mod pinned below 13 keeps the first registration, as before. Postfx
  truncation keeps the highest-priority passes.
- Mod SDK: a refine callback's `-1` on the moby channel means "not mine" for every mod: the moby passes to the next
  material in priority order that matches it, where it used to draw stock and hide every lower-priority material.
  `material_register_refine` and `material_set_selector` refuse, with one log line and -1, a refine that cannot take
  effect (a channel other than moby, terrain or any; a decal shader; a terrain selector with neither a texture key nor
  `OP_SEL_UNTEXTURED`). A moby selector with `OP_SEL_HP_TIER` no longer matches. A refine is asked once per tick.
- Mod SDK: a terrain refine's `-1` means "not mine" as on the moby channel: the face passes to the next refine keyed
  to it, then to the highest-priority declarative material matching it, where it used to draw stock. A textured face
  of an imported sector (no texture id of its own) now matches `OP_SEL_TEXTURED` materials in the retained terrain
  cache as it does in the walked terrain, where the cache treated it as untextured.
- Settings menu: a line above **EXIT GAME** names what the selected row changes. `--help` ends with the
  documentation's address.
- Settings menu: a **FOG** row below **LOD** sets `--fog`: **AUTO** (follows the LOD), **ORIGINAL**, **PER-PIXEL**
  or **OFF**.
- Mod SDK: a mod can read the discs of other Spyro games in the library. The manifest keys `requires_games` (the
  mod does not load while a listed game has no disc in the library, and the log, the Mods section and the launcher's
  version selector name the game) and `optional_games` (the mod loads; the disc calls for a missing game fail) name
  them. `read_sector` takes a game id (`OPENPETE_MOD_GAME_SPYRO1`, `_SPYRO2`, `_SPYRO3`, or `game_id`), and
  `game_disc_path` and `game_installed` are new.
- Spyro 3: a code mod's change to the program code the game's anti-piracy checks hash is reverted, and the log
  names the mod. `--scan-guard 0` (config key `scan_guard`, variable `OPENPETE_SCAN_GUARD`) lets the change stay;
  the flag has no effect on Spyro 1 and 2.
- Mod SDK: `antipiracy_scan` reads the state of Spyro 3's anti-piracy checks (the power-on, title and level-load
  checks, and each level's checking characters and objects) and the game's own pass or fail verdict, read only;
  `OPENPETE_MOD_ERR_UNSUPPORTED` on Spyro 1 and 2. The example mod `s3-antipiracy-sentinel` (formerly
  `s3-scan-markers`) colours each checking object by its check's state, lists the other checks in the mods panel,
  and can show a notification when a check fails.
- Mod SDK: `OPENPETE_MOD_ERR_UNSUPPORTED` (-2) is returned by `read_level_data` and `moby_classes` on a game that
  does not provide them; every other return value is unchanged.
- Mod SDK: `draw_text` draws on Spyro 2 and 3, in the game's own font.
- Mod SDK: materials (`[[material]]` rows and `material_register`) apply on every render channel of Spyro 2 and 3,
  and `moby_classes` works on Spyro 3, where class ids run to 767. `material_register` and
  `material_register_refine` now refuse a channel the running game does not serve, with one log line and -1, as
  `[[material]]` rows on that channel are skipped.
- Mod SDK: `vstream_handle` is Spyro 1 only. A mod built for Spyro 2 or 3 that uses it no longer compiles, with
  an error naming the alternative, moving the stream through the animation header's pointers; a mod built for
  every game gets 0 and one log line on Spyro 2 and 3.
- Mod SDK: the headers at the root of `sdk/` now include the selected game's headers from `sdk/games/<game>/`;
  a build line using `-I sdk` alone still compiles as Spyro 1, and naming a game the SDK does not carry stops
  with an error naming that game.
- A setting in `openpete.toml` or an `OPENPETE_*` environment variable whose value is the wrong type or out of range
  (`ssaa = 0`, `lod = "turbo"`, `OPENPETE_MOUSE=maybe`) no longer stops the game from starting: it prints a warning
  and the setting keeps its default. The same value typed on the command line, and any refused value under
  `--config-report`, still exits with an error.
- An unknown key in `openpete.toml`, an unknown flag or a key that is not a flag in its `[args]` table, and an
  unknown action, binding or table under `[keys]` no longer stop the game from starting: each prints a warning
  naming the file, the table and the key, and is ignored. `--config-report` still
  exits with an error on it; an unknown flag typed on the command line still does.
- `OPENPETE_WIREFRAME`, `OPENPETE_TERRAIN_CACHE`, `OPENPETE_DISC_CACHE`, `OPENPETE_LOAD_DELAY` and
  `OPENPETE_TERRAIN_VISIBILITY` set from an `[env]` table in `openpete.toml` take effect.
- `--help` lists `--ingest`, `--compile-mods`, `--log-file` and `--no-log-file` with the player-facing flags, and
  `--psycross-native-raster` with the debug flags. `--run-recomp` is removed; its spelling is the environment
  variable `OPENPETE_RUN_RECOMP`.
- Spyro 2 and 3: `--keys-report` prints every key action and its binding.
- Spyro 1 and 2: `--skip-to-cutscene` plays the story cutscenes by name (`--skip-to-cutscene zzz` lists them). Spyro 3: `--skip-to-cutscene` plays the 19 story cutscenes by their loading captions, and `--skip-to-area <level>:<area>` boots into a sub-area. `tests/harness/check_skip_targets.sh` and the `skip_targets_spyro-N` ctests run every row of `games/<game>/corpus/skip-targets.manifest`.
- Renderer: scene depth is now reversed on the float depth format (`gl_FragCoord.z` = 1/viewZ, larger is nearer),
  so distant surfaces no longer tie and flicker by draw order. Mod shaders reading depth through
  `op_viewz_scene` or `op_vz_from_depth` are unaffected. The constants `OP_DEPTH_AFFINE` and
  `DEPTH_AFFINE` are renamed `OP_DEPTH_REVERSED` and `DEPTH_REVERSED`; the old names stay as aliases.
  Raw `gl_FragCoord.z` values are not a stable interface.
- Spyro 1: the fog-band fade of distant characters follows every displayed frame with interpolation active
- Spyro 2/3: gem glints keep their size and spin between ticks; zaps keep their width; tracer ribbons and
  meshes move smoothly; the terrain's animated colours blend between ticks
- Spyro 3: terrain sectors no longer vanish on in-between frames while the camera pans under
  `--terrain-visibility game`
- Spyro 3: `--skip-to-level` gains a movie of the New Game path for the test corpus
- Tests: the 60 harness scripts and `spyro-records-corpus.txt` moved from `scripts/<x>` to
  `tests/harness/<x>`, and the corpus manifests from `games/<game>/<x>.manifest` to
  `games/<game>/corpus/<x>.manifest`; the oracle gate `check_psx_oracle.sh` moved with them,
  its flags, exit codes and golden capture unchanged

## v0.4.3 - 2026-10-29 (Current)

- Fixed a crash at startup on AMD Radeon RX 9000-series graphics cards (RDNA4)

## v0.4.2 - 2026-09-29

- Spyro 1: Fixed Sparx, gems and other mobys near Spyro drawing behind him when they are in front of him

## v0.4.1 - 2026-09-22

- Spyro 1: Fixed the HUD eggs swimming between each other with interpolation active
- Spyro 1: Fixed a black line along the bottom of the screen on the pause menu

## v0.4.0 - 2026-09-22

- Fixed subtle flickering with interpolation active
- Added field of view (FOV) option
- Split out wobble and pixelation options
- Reduced VRAM usage
- Fixed the dev menu (default: <kbd>M</kbd>) opening and closing the first time
- Implemented mouse support for the in-game menu, freecam and bindings (<code>mouse:left</code>, <code>mouse:wheel_up</code>, <code>mouse:x</code>)
- Mod SDK: Added [mouse APIs](https://openpete.com/docs/modding/reference/api.html#mouse)
- Mod SDK: Added [controller APIs](https://openpete.com/docs/modding/reference/api.html#pads)
- Mod SDK: Added [api->disc_path](https://openpete.com/docs/modding/reference/api.html#api-disc_path)
- Mod SDK: Dynamically allocate asset arrays to remove fixed capacities
- Mod SDK: Added PSYQ types in the generated struct header with a <code>PSX\_</code> prefix
- Spyro 1: Fixed some near-camera terrain rendering the low-poly geometry in the end credits
- Spyro 1: Fixed the interpolation of text mobys in the end credits
- Spyro 1: Fixed big boi mobys being culled near the edges of the screen in widescreen
- Spyro 1: Fixed the bulls in town square shrinking when changing animation poses with interpolation active
- Spyro 1: Fixed fanning chests' fans rendering pure black at large distances with extended LOD
- Spyro 1: Fixed 2 missing polys on Stone Hill with extended LOD
- Spyro 1: Fixed the green pause framebuffer effect not updating with wobble enabled
- Spyro 1: Fixed moby positioning being slightly off with interpolation active

## v0.3.0 - 2026-09-10

- Fixed a crash when pressing <kbd>F12</kbd> to screenshot
- Spyro 1: Fixed mobys incorrectly appearing at the edges of the screen during dragon cutscenes in widescreen
- Linux: Bundle `libsndio` in the AppImage distribution
- Mod SDK: Full support for ImGui via [`dear_bindings`](https://github.com/dearimgui/dear_bindings)
- Mod SDK: Fixed shader materials rendering corruption (white flashing) caused by a race between the simulation and presentation threads
- Mod SDK: Support dragon, gem, and egg counts in custom levels
- Mods: Fixed installing a `.zip` made by compressing the mod folder itself, where `mod.toml` sits inside a wrapper directory
- Fixed post-processing passes reading the wrong parameters, slightly mis-shading a few pixels per-frame
- Support overriding `gamecontrollerdb.txt` by `--controller-db`, `OPENPETE_CONTROLLER_DB`, `controller_db` in `openpete.toml`, or `$XDG_CONFIG_HOME/openpete/gamecontrollerdb.txt` in the Linux AppImage distribution
- Updated `gamecontrollerdb.txt` to latest
- Support for up to 4 controllers
- Removed SSAA &times;2 from the default configuration

## v0.2.0 - 2026-09-04

- Mod SDK: Initial release
- Linux: Initial release of AppImage distribution
- Improved overall performance by 2-5x, depending on scene and settings
- Significantly improved sim-tick pacing and consistency (99.75% of ticks begin within 250 µs of the target)
- Significantly improved frame pacing and consistency
- Significantly reduced animation error
- Improved texture pack performance
- Improved relative audio volume and stereo mixing
- The game no longer pauses simulation when the window is dragged, resized or fullscreen is toggled
- Improved input handling and responsiveness
- The cursor now hides when inside the game window
- Added a notifications OSD
- Added support for controller vibration
- Spyro 1: Fixed the portal cyclorama not interpolating when entering a portal
- Spyro 1: Fixed Sparx's glow being incorrectly visible in dragon rescue cutscenes
- Various other graphical fixes and improvements
- Spyro 1: Added an easter egg when entering the Crash 3 demo code (instead of just *crash*ing)

## v0.1.4 - 2026-08-17

- Spyro 1: Fixed semi-transparent terrain showing through the ground and through the dragon during rescue cutscenes
- Lowered the CPU requirement to plain x86-64, so pre-2013 processors without AVX2 can run the build
- Fixed in-game menu not saving certain settings

## v0.1.3 - 2026-08-14

- Spyro 1: Fixed a crash when opening a locked chest in Lofty Castle. The same issue caused silent memory corruption in Peace Keepers, Dry Canyon and Tree Tops
- Capped SSAA to 4&times;

## v0.1.2 - 2026-08-13 (Initial release)

- Initial public release
