# core/rhi — game-agnostic Render Hardware Interface (thin) + Vulkan backend.
#
# The native renderer's foundation. Lives under core/ because it is both
# game-agnostic and platform-agnostic (windowing/surface via SDL, never a
# platform header). See notes/plans/openpete-native-renderer.md Phase 1.
#
# Vulkan is loaded dynamically via volk (third_party/volk): it dlopens
# libvulkan.so.1 / LoadLibrary("vulkan-1.dll") at runtime, so NO Vulkan
# loader import library is linked on any platform and the Windows
# cross-build needs no Vulkan SDK — only the vendored headers
# (third_party/vulkan-headers). The rhi.h seam keeps Metal/WebGPU additive.

set(VULKAN_HEADERS_INC ${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include)
set(VOLK_DIR           ${CMAKE_SOURCE_DIR}/third_party/volk)

# Shaders: GLSL -> SPIR-V via glslangValidator (a host build tool; .spv is
# portable, so this is identical for native and cross builds).
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
set(RHI_SHADER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(RHI_SHADER_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${RHI_SHADER_OUT_DIR})
set(RHI_SHADERS quad.vert quad.frag psx_sample.vert psx_sample.frag blit.frag)
set(RHI_SHADER_SPV "")
foreach(s IN LISTS RHI_SHADERS)
    add_custom_command(
        OUTPUT ${RHI_SHADER_OUT_DIR}/${s}.spv
        COMMAND ${GLSLANG_VALIDATOR} -V ${RHI_SHADER_SRC_DIR}/${s}
                -o ${RHI_SHADER_OUT_DIR}/${s}.spv
        DEPENDS ${RHI_SHADER_SRC_DIR}/${s}
        COMMENT "glslangValidator ${s} -> ${s}.spv"
        VERBATIM)
    list(APPEND RHI_SHADER_SPV ${RHI_SHADER_OUT_DIR}/${s}.spv)
endforeach()
add_custom_target(openpete_rhi_shaders DEPENDS ${RHI_SHADER_SPV})

# Export the compiled .spv paths so per-game targets can stage them next to
# their executable with a proper dependency (re-stages on shader change, not
# just on relink). PARENT_SCOPE writes into the top-level scope, which sibling
# subdirectories added later (games/<game>/) inherit.
set(RHI_SHADER_SPV "${RHI_SHADER_SPV}" PARENT_SCOPE)

add_library(openpete_rhi STATIC
    rhi_vulkan.c
    ${VOLK_DIR}/volk.c
)
target_include_directories(openpete_rhi
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${VULKAN_HEADERS_INC} ${VOLK_DIR}
)
target_compile_definitions(openpete_rhi
    PUBLIC  RHI_SHADER_DIR="${RHI_SHADER_OUT_DIR}"
    PRIVATE VK_NO_PROTOTYPES)
# volk dlopens the loader; on Linux that needs libdl (CMAKE_DL_LIBS), on
# Windows it uses LoadLibrary (no extra lib).
target_link_libraries(openpete_rhi PUBLIC SDL3::SDL3-static ${CMAKE_DL_LIBS})
set_target_properties(openpete_rhi PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)
add_dependencies(openpete_rhi openpete_rhi_shaders)

# Milestone 1a/1b smoke test: a standalone Vulkan window (1a cleared the
# offscreen target; 1b.1 also draws a vertex-colored quad). Verifies the
# bringup without the rest of openpete. On by default while the foundation
# is under construction; off via -DOPENPETE_BUILD_RHI_SMOKETEST=OFF.
option(OPENPETE_BUILD_RHI_SMOKETEST "Build the RHI smoke test exe" ON)
if(OPENPETE_BUILD_RHI_SMOKETEST)
    add_executable(rhi_smoketest smoketest.c)
    target_link_libraries(rhi_smoketest PRIVATE openpete_rhi SDL3::SDL3-static)
endif()
