# macos-universal: placeholder (not validated). CMAKE_OSX_ARCHITECTURES
# stays in CMakePresets.json — it must be set BEFORE project() runs the
# compiler checks, and this file is included at the end of project().
include(${CMAKE_CURRENT_LIST_DIR}/_helpers.cmake)
set(CMAKE_BUILD_TYPE RelWithDebInfo)
