# wasm: Emscripten / browser placeholder - trim non-browser components.
include(${CMAKE_CURRENT_LIST_DIR}/_helpers.cmake)
set(CMAKE_BUILD_TYPE Release)
openpete_preset_default(OPENPETE_SKIP_DECOMP ON)
openpete_preset_default(OPENPETE_BUILD_TESTS OFF)
