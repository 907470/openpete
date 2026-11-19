# Save-record state model (pure C rules, tests/modsave/modsave_model.c).
#
# modsave_model_test runs the design's worked cases (each event's expected
# effects written from the design) on the C module. modsave_state_model
# runs the exhaustive search (state_search.py): every suite breadth-first
# to its quick depth, the C module and the Python reference model
# (model_ref.py) applied to every edge, effects and canonical shapes
# compared, invariants checked; the slow variant runs the full depths.
# modsave_state_model_indep runs it against the second reference,
# model_ref_indep.py. The search loads the module through ctypes from
# modsave_model_py.
# Timeouts are twice the measured wall time (quick 49 s, full 326 s).
# Every test here carries the `unit` label, as in tests/unit; the full-depth
# variants are also `slow`, which the landing's unit set excludes.
cmake_language(DEFER CALL openpete_label_directory_tests unit)

add_library(modsave_model STATIC modsave_model.c)
target_include_directories(modsave_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(modsave_model PROPERTIES C_STANDARD 11)

add_library(modsave_model_py SHARED modsave_model.c)
set_target_properties(modsave_model_py PROPERTIES C_STANDARD 11)

add_executable(modsave_model_test modsave_model_test.c)
target_link_libraries(modsave_model_test PRIVATE modsave_model)
set_target_properties(modsave_model_test PROPERTIES C_STANDARD 11)
add_test(NAME modsave_model_test COMMAND modsave_model_test)
set_tests_properties(modsave_model_test PROPERTIES LABELS "mods")

add_test(NAME modsave_state_model
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/state_search.py
                 --lib $<TARGET_FILE:modsave_model_py> --quick
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(modsave_state_model PROPERTIES LABELS "mods" TIMEOUT 120)

add_test(NAME modsave_state_model_full
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/state_search.py
                 --lib $<TARGET_FILE:modsave_model_py>
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(modsave_state_model_full PROPERTIES LABELS "slow;mods" TIMEOUT 660)

# The same search against model_ref_indep.py, the second reference written
# apart from model_ref.py and the C module (quick 45 s, full 372 s).
add_test(NAME modsave_state_model_indep
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/state_search.py
                 --lib $<TARGET_FILE:modsave_model_py> --quick --ref indep
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(modsave_state_model_indep PROPERTIES LABELS "mods" TIMEOUT 120)

add_test(NAME modsave_state_model_indep_full
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/state_search.py
                 --lib $<TARGET_FILE:modsave_model_py> --ref indep
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(modsave_state_model_indep_full PROPERTIES LABELS "slow;mods" TIMEOUT 750)

# Calibration (LAW §13): each seeded rule defect must be reported by the
# check that guards it; the test passes only when it is.
foreach(_fault 1 2 3 4)
    add_test(NAME modsave_state_model_fault${_fault}
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/state_search.py
                     --lib $<TARGET_FILE:modsave_model_py> --quick --fault ${_fault}
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(modsave_state_model_fault${_fault} PROPERTIES LABELS "mods" TIMEOUT 600)
endforeach()
