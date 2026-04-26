# tests/unit/CMakeLists.txt — host-side unit tests.
#
# These don't boot the PSX runtime; they exercise pure functions
# (parsers, decoders) that we'd rather catch regressions in without a full
# game build. Hooked into ctest via enable_testing() in the root list.

add_executable(bk2_movie_test bk2_movie_test.c)
target_link_libraries(bk2_movie_test PRIVATE openpete_core)
add_test(NAME bk2_movie_test COMMAND bk2_movie_test)
