# The build-directory lock (scripts/lib/build_lock.py): every Ninja build
# holds <build dir>/.openpete-build.lock exclusive from its first step until
# the build driver exits, and a gate holds it shared while it reads the
# build's binary. openpete_build_lock_all() adds the target that takes it
# and makes every other target of the build depend on it, so no step of any
# target runs before the lock is held. The dependencies are order-only: the
# target runs in every build and dirties nothing.

function(_openpete_build_lock_targets dir out)
    get_property(_t DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_subs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_s IN LISTS _subs)
        _openpete_build_lock_targets("${_s}" _st)
        list(APPEND _t ${_st})
    endforeach()
    set(${out} ${_t} PARENT_SCOPE)
endfunction()

function(openpete_build_lock_all)
    if(NOT CMAKE_HOST_UNIX OR NOT CMAKE_GENERATOR MATCHES "Ninja")
        return()
    endif()
    if(Python3_EXECUTABLE)
        set(_py "${Python3_EXECUTABLE}")
    else()
        set(_py python3)
    endif()
    add_custom_target(openpete_build_lock
        COMMAND "${_py}" "${CMAKE_SOURCE_DIR}/scripts/lib/build_lock.py"
                build-hold --dir "${CMAKE_BINARY_DIR}"
        COMMENT "Holding ${CMAKE_BINARY_DIR} against gates for this build"
        USES_TERMINAL VERBATIM)
    _openpete_build_lock_targets("${CMAKE_SOURCE_DIR}" _all)
    foreach(_t IN LISTS _all)
        if(_t STREQUAL "openpete_build_lock")
            continue()
        endif()
        get_target_property(_type ${_t} TYPE)
        if(_type STREQUAL "INTERFACE_LIBRARY")
            continue()
        endif()
        add_dependencies(${_t} openpete_build_lock)
    endforeach()
endfunction()
