# ios-arm64: iOS device build (first milestone: engine only; see
# .github/workflows/ios.yml). CMAKE_SYSTEM_NAME lives in
# cmake/toolchains/ios.cmake because it must be set before project().
include(${CMAKE_CURRENT_LIST_DIR}/_helpers.cmake)
set(CMAKE_BUILD_TYPE Release)

# iOS forbids loading unsigned code at runtime and ships no compiler, so the
# recompiled module must be linked into the executable ahead of time.
openpete_preset_default(OPENPETE_RECOMP_LINKAGE static)
openpete_preset_default(OPENPETE_RECOMP_FIRST_BOOT OFF)

# Tests launch the built executables, which cannot run on a CI host.
openpete_preset_default(OPENPETE_BUILD_TESTS OFF)
# --record needs libav; there is none for iOS.
openpete_preset_default(OPENPETE_ENABLE_RECORD OFF)
# Mod compilation invokes the exe (native) or a MinGW cross compiler.
openpete_preset_default(OPENPETE_COMPILE_MODS OFF)
