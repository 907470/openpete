# Mirror the repo's mods/ tree into the cross build's <exe>/mods/ so
# `cmake --build` refreshes staged mod *sources* the same way it
# refreshes staged DLLs (games/spyro-1/CMakeLists.txt cross branch) —
# closing the staged-source-rot gap where a hand-copied mod's src/
# drifted from the repo. Overlay rules, per repo mod <SRC>/<id>/:
#   - exact mirror (copy-if-different + prune deleted files), EXCEPT:
#   - dot-dirs (.build/ artifacts, .unpacked/) are runtime-owned: never
#     copied, never pruned;
#   - config.toml is user-owned (hot-reload watched; the player tunes
#     the staged copy): copied only if missing.
# Staged mod dirs with no repo counterpart are left alone — that's
# where a user drops third-party mods next to the exe. enabled.txt is
# user-owned play state: copied only if missing.
# Usage: cmake -DSRC=<repo mods dir> -DDST=<build mods dir> -P stage_mods.cmake
file(GLOB _manifests ${SRC}/*/mod.toml)
foreach(_manifest IN LISTS _manifests)
    get_filename_component(_src_dir ${_manifest} DIRECTORY)
    get_filename_component(_id ${_src_dir} NAME)
    set(_dst_dir ${DST}/${_id})

    file(GLOB_RECURSE _files RELATIVE ${_src_dir} ${_src_dir}/*)
    list(FILTER _files EXCLUDE REGEX "(^|/)\\.")
    foreach(_rel IN LISTS _files)
        get_filename_component(_parent ${_dst_dir}/${_rel} DIRECTORY)
        file(MAKE_DIRECTORY ${_parent})
        if(_rel STREQUAL "config.toml")
            if(NOT EXISTS ${_dst_dir}/${_rel})
                file(COPY_FILE ${_src_dir}/${_rel} ${_dst_dir}/${_rel})
            endif()
        else()
            file(COPY_FILE ${_src_dir}/${_rel} ${_dst_dir}/${_rel}
                 ONLY_IF_DIFFERENT)
        endif()
    endforeach()

    # Prune staged files whose repo counterpart is gone (a deleted
    # src/*.c would otherwise still reach a Windows-host recompile).
    file(GLOB_RECURSE _staged RELATIVE ${_dst_dir} ${_dst_dir}/*)
    list(FILTER _staged EXCLUDE REGEX "(^|/)\\.")
    foreach(_rel IN LISTS _staged)
        if(NOT _rel STREQUAL "config.toml" AND NOT EXISTS ${_src_dir}/${_rel})
            file(REMOVE ${_dst_dir}/${_rel})
        endif()
    endforeach()
endforeach()

if(EXISTS ${SRC}/enabled.txt AND NOT EXISTS ${DST}/enabled.txt)
    file(MAKE_DIRECTORY ${DST})
    file(COPY_FILE ${SRC}/enabled.txt ${DST}/enabled.txt)
endif()
