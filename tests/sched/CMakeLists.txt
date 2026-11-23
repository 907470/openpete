# tests/sched - deterministic interleaving scheduler (lib/) and its
# calibration on toy programs (toys/).
#
# The scheduler replaces SDL's synchronisation, thread and clock calls by
# -Wl,--wrap and the ThreadSanitizer runtime by its own __tsan_* entry
# points: code under test is compiled with -fsanitize=thread, libtsan is
# never linked, and SDL itself is not linked either, so a call the wraps do
# not cover is a link error. Linux only (GNU ld --wrap).
if(WIN32 OR NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    return()
endif()

cmake_language(DEFER CALL openpete_label_directory_tests unit)
find_package(Threads REQUIRED)

set(SCHED_SDL_WRAPS
    SDL_CreateMutex SDL_DestroyMutex SDL_LockMutex SDL_TryLockMutex SDL_UnlockMutex
    SDL_CreateCondition SDL_DestroyCondition SDL_SignalCondition SDL_BroadcastCondition
    SDL_WaitCondition SDL_WaitConditionTimeout
    SDL_CreateThreadRuntime SDL_WaitThread SDL_DetachThread SDL_GetCurrentThreadID
    SDL_GetTicksNS SDL_GetTicks SDL_DelayNS SDL_Delay
    SDL_GetAtomicInt SDL_SetAtomicInt SDL_AddAtomicInt SDL_CompareAndSwapAtomicInt)
set(SCHED_WRAP_LINK "")
foreach(sym IN LISTS SCHED_SDL_WRAPS)
    list(APPEND SCHED_WRAP_LINK "-Wl,--wrap=${sym}")
endforeach()
set(SCHED_WRAP_LINK ${SCHED_WRAP_LINK} PARENT_SCOPE)

add_library(openpete_sched STATIC
    lib/sched.c lib/explore.c lib/sdl_wraps.c lib/tsan_hooks.c)
target_include_directories(openpete_sched PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lib)
target_compile_options(openpete_sched PRIVATE -Wall -Wextra)
set_target_properties(openpete_sched PROPERTIES C_STANDARD 11)
target_link_libraries(openpete_sched PUBLIC SDL3::Headers Threads::Threads ${CMAKE_DL_LIBS})
target_link_options(openpete_sched INTERFACE ${SCHED_WRAP_LINK})

# The toy whose shared accesses the hooks see.
add_library(sched_toy_tsan OBJECT toys/toy_counter.c)
target_compile_options(sched_toy_tsan PRIVATE -fsanitize=thread -O2)

add_executable(sched_calibrate toys/sched_calibrate.c $<TARGET_OBJECTS:sched_toy_tsan>)
target_include_directories(sched_calibrate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/toys)
target_compile_options(sched_calibrate PRIVATE -Wall -Wextra)
set_target_properties(sched_calibrate PROPERTIES C_STANDARD 11)
target_link_libraries(sched_calibrate PRIVATE openpete_sched)
add_test(NAME sched_calibrate
         COMMAND sched_calibrate ${CMAKE_CURRENT_BINARY_DIR}/calibrate 4)
add_subdirectory(present)
