# Spyro 2 — port placeholder

No matching decomp project has been identified yet. Once one exists (or once
this codebase grows the surface area to bring up Spyro 2 from raw EXE), wire
it up here per the same pattern as `games/spyro-1/`:

- submodule the decomp under `games/spyro-2/spyro-2/`
- create `games/spyro-2/manifest/`, `games/spyro-2/generated/`
- write `games/spyro-2/game_extras_impl.c` filling in entry point, CRC, etc.
- write `games/spyro-2/CMakeLists.txt` modeled after Spyro 1's
- enable in the top-level `CMakeLists.txt`'s `OPENPETE_GAMES` list

See `docs/adding-a-game.md` for the full checklist.
