# Shared helper for cmake/presets/<preset>.cmake files.
#
# These files exist to kill the sticky-cache-variable footgun (see
# "Cache variables are sticky" in docs/src/developer/building.md): CMakePresets.json
# pins ONE immutable cache entry per preset — CMAKE_PROJECT_openpete_INCLUDE
# pointing at the preset's .cmake file — and every knob lives here as a
# tracked NORMAL variable, re-evaluated at the end of project() on EVERY
# reconfigure. Editing a preset file takes effect on the next plain
# `cmake --build` in every build dir and worktree (the include is a
# configure regen dependency); adding or removing a knob needs no
# `cmake --preset` re-run and leaves nothing behind in CMakeCache.txt.
#
# openpete_preset_default(VAR VALUE) applies a knob unless the user
# deliberately overrode it with `-DVAR=...` — detected by the fixed
# HELPSTRING CMake stamps on command-line cache entries. A mere cache
# entry is NOT enough to defer to: option()/set(CACHE) declarations
# back-fill their defaults into the cache on any configure where the
# preset file didn't set the knob, so a knob ADDED to a preset file
# later would lose to that stale default forever (the very footgun
# these files exist to kill). The -D override is the one remaining
# sticky surface: drop it with `cmake -U VAR <build-dir>` when done.
# Requires CMP0077/CMP0126 NEW (top-level cmake_minimum_required 3.22):
# option()/set(CACHE) consumers see the normal variable and leave it
# alone.
macro(openpete_preset_default _var _val)
    set(_op_cli_override FALSE)
    if(DEFINED CACHE{${_var}})
        get_property(_op_helpstring CACHE ${_var} PROPERTY HELPSTRING)
        if(_op_helpstring STREQUAL
           "No help, variable specified on the command line.")
            set(_op_cli_override TRUE)
        endif()
    endif()
    if(NOT _op_cli_override)
        set(${_var} "${_val}")
    endif()
endmacro()
