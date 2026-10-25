# games/common/CMakeLists.txt - the host code every game links, compiled once.
#
# openpete_game_common holds the sources every game's executable links: the
# process entry and game selection (game_entry.cpp, game_pick.cpp,
# game_build.c), the entry
# of a game with none of its own (game_main.cpp), the shared PSYQ-to-PsyCross
# layer with its per-game hooks forwarded to the running game's descriptor,
# the freecam body, the censuses, and the shared renderer TUs (the GTE float
# mirror, the render-path isolation registry, the feeders reached only
# through a game's feature list, the orchestrator entries forwarded to the
# game's descriptor). A games/common source a game names beyond these
# compiles once as its own object library (_openpete_common_object,
# cmake/openpete_game.cmake). Every one of them compiles with the
# game-neutral include path: games/common and core's.

set(OPENPETE_GAME_COMMON_SOURCES
    game_entry.cpp
    game_pick.cpp
    game_build.c
    game_main.cpp
    game_extras_default.c
    game_host_desc.c

    # The shared PSYQ-to-PsyCross layer and its per-game hooks, forwarded
    # to the game's descriptor.
    psyq/psyq_thunks.cpp
    psyq/psyq_game_hooks.c
    psyq/psyq_sfx_slots.c
    psyq/psyq_thunk_state.c
    write_census.c
    dispatch_census.c
    dispatch_preamble.c
    freecam_game.c
    named_target.c

    # Shared renderer TUs: the GTE float mirror every feeder projects
    # through, the --render-paths isolation registry, and the terrain,
    # cyclorama, moby and moby-shadow feeders (reached only through a game's
    # feature list).
    renderer/native_gte.c
    renderer/orchestrator.c
    renderer/hook_bracket.c
    renderer/render_paths.c
    renderer/portal_plane.c
    renderer/terrain.c
    renderer/cyclorama.c
    renderer/shadow.c
    renderer/shadow_player.c
    renderer/flame.c
    # The glow/glint/tracer effect family: five self-linking producers,
    # registered only by a game whose game_layout names their routines.
    renderer/glow.c
    renderer/glint.c
    renderer/zap.c
    renderer/tracers.c
    renderer/particles.c
    renderer/player_hide.c
    renderer/moby.c
    renderer/moby_rank.c
    renderer/moby_rank_cal.c
    renderer/moby_snap.c
    renderer/player.c
    renderer/water_plane.c
    renderer/player_ranges.c
    renderer/ui_sprite.c
    # The SPRT text feeder: registered only by a game whose ui_game names a
    # self-linking chain emitter, inert in the others.
    renderer/text_sprt.c
    renderer/interp_regions_gen.c
    renderer/credits_key.c
    renderer/reextract_view.c
    renderer/cut_guard.c
    # OPENPETE_D1: every moby updated, reached only through a game's
    # feature list.
    features/moby_update_all.c)

add_library(openpete_game_common OBJECT ${OPENPETE_GAME_COMMON_SOURCES})
openpete_game_common_settings(openpete_game_common)
