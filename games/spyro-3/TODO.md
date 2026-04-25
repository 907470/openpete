# Spyro 3 — port placeholder

Spyro 3's decomp is early-stage. We're tracking it but no integration work
is planned until the matching decomp's `psx.map` is stable enough to drive a
recompile. When that happens, wire it up the same way as `games/spyro-1/`:

- submodule the decomp under `games/spyro-3/spyro-3/`
- create `games/spyro-3/manifest/`, `games/spyro-3/generated/`
- write `games/spyro-3/game_extras_impl.c` filling in entry point, CRC, etc.
- write `games/spyro-3/CMakeLists.txt` modeled after Spyro 1's
- enable in the top-level `CMakeLists.txt`'s `OPENPETE_GAMES` list

See `docs/adding-a-game.md` for the full checklist.
