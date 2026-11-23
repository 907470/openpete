# tests/sched/present - the present-thread interleaving harness: the real
# present_thread.c, the scene ring copied from native_render.c and the real
# rhi_vulkan.c over a fake Vulkan device, under the scheduler of ../lib.
#
# The protocol objects are test copies compiled with -fsanitize=thread (the
# scheduler's __tsan_* entry points, no libtsan); objcopy makes the statics
# the harness reads global. Release objects are not touched.
cmake_language(DEFER CALL openpete_label_directory_tests unit)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(PT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(PT_GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${PT_GEN})

add_custom_command(
    OUTPUT ${PT_GEN}/ring_extract.c ${PT_GEN}/pt_state.h
    COMMAND ${Python3_EXECUTABLE} ${PT_DIR}/extract_ring.py
            ${CMAKE_SOURCE_DIR}/core/renderer/native_render.c
            ${CMAKE_SOURCE_DIR}/core/runtime/present_thread.c
            ${PT_GEN}/ring_extract.c ${PT_GEN}/pt_state.h
    DEPENDS ${PT_DIR}/extract_ring.py
            ${CMAKE_SOURCE_DIR}/core/renderer/native_render.c
            ${CMAKE_SOURCE_DIR}/core/runtime/present_thread.c
    COMMENT "present_sched: copying the scene ring and the present thread's state"
    VERBATIM)
add_custom_target(present_sched_gen DEPENDS ${PT_GEN}/ring_extract.c ${PT_GEN}/pt_state.h)

set(PT_INCLUDES
    ${PT_DIR} ${PT_GEN} ${CMAKE_CURRENT_SOURCE_DIR}/../lib
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt
    ${CMAKE_SOURCE_DIR}/core/host
    ${CMAKE_SOURCE_DIR}/core/rhi
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include
    ${CMAKE_SOURCE_DIR}/third_party/volk)
set(PT_TSAN -fsanitize=thread -O2 -g)

# The protocol objects, instrumented.
add_library(present_sched_pt OBJECT ${CMAKE_SOURCE_DIR}/core/runtime/present_thread.c)
target_include_directories(present_sched_pt PRIVATE ${PT_INCLUDES})
target_compile_definitions(present_sched_pt PRIVATE VK_NO_PROTOTYPES OPENPETE_ENGINE_BUILD)
target_compile_options(present_sched_pt PRIVATE ${PT_TSAN})
target_link_libraries(present_sched_pt PRIVATE SDL3::Headers)
set_target_properties(present_sched_pt PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)

add_library(present_sched_rhi OBJECT ${CMAKE_SOURCE_DIR}/core/rhi/rhi_vulkan.c)
target_include_directories(present_sched_rhi PRIVATE ${PT_INCLUDES})
target_compile_definitions(present_sched_rhi PRIVATE VK_NO_PROTOTYPES)
target_compile_options(present_sched_rhi PRIVATE ${PT_TSAN})
target_link_libraries(present_sched_rhi PRIVATE SDL3::Headers)
set_target_properties(present_sched_rhi PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)

# The statics the harness reads, made global on the test copies.
add_custom_command(
    OUTPUT ${PT_GEN}/present_thread.sched.o
    COMMAND ${CMAKE_OBJCOPY} --globalize-symbol=s_pt --globalize-symbol=s_staged --globalize-symbol=s_lat
            $<TARGET_OBJECTS:present_sched_pt> ${PT_GEN}/present_thread.sched.o
    DEPENDS present_sched_pt $<TARGET_OBJECTS:present_sched_pt>
    COMMAND_EXPAND_LISTS VERBATIM)
add_custom_command(
    OUTPUT ${PT_GEN}/rhi_vulkan.sched.o
    COMMAND ${CMAKE_OBJCOPY} --globalize-symbol=s_display_owner
            $<TARGET_OBJECTS:present_sched_rhi> ${PT_GEN}/rhi_vulkan.sched.o
    DEPENDS present_sched_rhi $<TARGET_OBJECTS:present_sched_rhi>
    COMMAND_EXPAND_LISTS VERBATIM)
add_custom_command(
    OUTPUT ${PT_GEN}/vk_list.h
    COMMAND ${Python3_EXECUTABLE} ${PT_DIR}/gen_vk_list.py ${CMAKE_NM} ${PT_GEN}/rhi_vulkan.sched.o ${PT_GEN}/vk_list.h
    DEPENDS ${PT_DIR}/gen_vk_list.py ${PT_GEN}/rhi_vulkan.sched.o
    VERBATIM)
set_source_files_properties(${PT_GEN}/present_thread.sched.o ${PT_GEN}/rhi_vulkan.sched.o
    PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

# One harness per ring size: the shipped ring, and a ring of 3 for ring pressure.
function(present_sched_harness name ring)
    add_executable(${name}
        ${PT_DIR}/harness.c ${PT_DIR}/fakevk.c ${PT_DIR}/stubs.c ${PT_GEN}/ring_extract.c ${PT_GEN}/vk_list.h
        ${PT_GEN}/present_thread.sched.o ${PT_GEN}/rhi_vulkan.sched.o)
    target_include_directories(${name} PRIVATE ${PT_INCLUDES})
    target_compile_definitions(${name} PRIVATE VK_NO_PROTOTYPES PT_SCENE_RING=${ring})
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    set_source_files_properties(${PT_GEN}/ring_extract.c TARGET_DIRECTORY ${name} PROPERTIES COMPILE_OPTIONS "${PT_TSAN}")
    set_target_properties(${name} PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
    add_dependencies(${name} present_sched_gen openpete_rhi)
    target_link_options(${name} PRIVATE -Wl,--wrap=rhi_display_claim)
    # The RHI's other objects (pipeline cache, memory types, GPU audit) from its archive, before
    # the scheduler's; no SDL, no libtsan.
    target_link_libraries(${name} PRIVATE $<TARGET_FILE:openpete_rhi> openpete_sched m)
endfunction()
present_sched_harness(present_sched 48)
present_sched_harness(present_sched_r3 3)

add_test(NAME present_sched
         COMMAND present_sched --out ${CMAKE_CURRENT_BINARY_DIR}/run
                 --sub $<TARGET_FILE:present_sched_r3>)
