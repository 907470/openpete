# Public-repo hygiene audit (distribution D3, 2026-07-07)

What the openpete repo may and may not carry when it goes public. The
distribution stance is strict: we ship no game bytes; the *source repo*
should hold itself to the same standard. Inventory of everything
game-derived that is currently **tracked**, with a verdict each.

## Findings

| Item | What it is | Verdict |
|---|---|---|
| `tests/golden/spyro-1/psxtrace-demo-20k.bin` | Per-tick **RAM captures** of gameplay-state regions from real hardware — literal game memory bytes (~large) | **REMOVE before public.** Move to a private release asset / CI secret store; `check_psx_oracle.sh` already SKIPs (exit 64) when absent. |
| `tests/golden/spyro-1/native-vk/*.png`, `psycross-ref/*.png` | Rendered game frames (copyrighted imagery) | **REMOVE before public** (same relocation). `check_native_visual.sh` skips when absent. |
| `games/spyro-1/bk2/*.bk2` | BizHawk **input recordings** — button presses only, no game data | **KEEP.** Inputs are ours. |
| `games/spyro-1/manifest/recomp/SCUS-94228/` | Converged forced-entry address lists + per-binary sha256es | **KEEP** — addresses/hashes are facts (the OpenGOAL posture). |
| `manifest/*.toml`, `asm_dispatch_targets/` | Authored metadata (addresses, symbol names) | **KEEP.** |
| `games/spyro-1/spyro-1` submodule ref | Points at the **private** decomp repo | **KEEP the gitmodules entry** (fetch fails gracefully); dist presets (`OPENPETE_ROM_FREE_ICON=ON`, `SKIP_DECOMP=ON`) must configure without it — icon was the last asset dependency (fixed); `setup.sh` and regen remain dev-only paths. |
| `games/spyro-1/spyro-1/assets/save_header.databin.bin` (via submodule) | Memcard header bytes used for the dev icon | Lives in the private submodule, not this repo — fine; dist builds use the ROM-free icon. |
| Untracked working files (`SPYROLOGO.png`, `screenshot-*.png`, `Spyro the Dragon.log`) | Local artifacts | Untracked — no action for the repo; **never** `git add` them (SPYROLOGO.png is the user's). |
| `docs/level-*.md`, `gte-trace-vs-reference.md` etc. | Format documentation — facts and structure, no content dumps | **KEEP.** |
| `third_party/` vendored code | Licensed upstream code with ORIGIN/PATCHES bookkeeping | **KEEP** (NOTICE.md covers it). |

## Action list before flipping public

1. Relocate `tests/golden/spyro-1/` binaries + PNGs to private storage
   (CI fetches them as a secret artifact; scripts already skip-when-absent).
   Purge them from git history (`git filter-repo`) — they are in past
   commits, not just HEAD.
2. Verify a fresh clone with no private-submodule access configures and
   builds `linux-release-shared` + `windows-mingw-shared` (needs a
   substitute for `games/spyro-1/spyro-1/build/*` inputs only at REGEN
   time, which is dev-only — but confirm CMake doesn't hard-require the
   submodule path at configure).
3. ~~Re-read `README.md` — the disc-location default references a local
   path (`~/...`); generalize before public.~~ **DONE
   2026-07-07** — `README.md` and `scripts/setup.sh` now use a
   repo-relative default (`<repo>/../roms/...`) keyed off `OPENPETE_DISC`.
4. License check: `--record` linking libx264 makes GPL builds — the
   packaged dist presets don't include libav on Windows; Linux dist
   currently links system libav (LGPL dylibs, fine) with x264 optional.
   Confirm the shipped binary's encoder set before release.

Everything else in the tree is authored code, facts-metadata, or
properly-attributed vendored source.
