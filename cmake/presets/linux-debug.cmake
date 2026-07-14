# linux-debug: native Debug, hybrid decomp mode (the ONLY preset that
# builds the WIP native-decomp TUs — OPENPETE_SKIP_DECOMP stays OFF).
include(${CMAKE_CURRENT_LIST_DIR}/_helpers.cmake)
set(CMAKE_BUILD_TYPE Debug)
