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
set(RHI_SHADERS quad.vert quad.frag psx_sample.vert psx_scene.vert psx_sample.frag psx_scene.frag blit.frag diff.frag freeze_overlay.frag resolve.frag overlay_resolve.frag shadow_decal.frag prepass.vert occl_downsample.frag inpaint.frag hd_tex.frag)
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
# Embed the compiled .spv into the binary: a build-time `cmake -P` step turns
# the SPIR-V into a generated C source (byte arrays + a name->blob table),
# which is compiled into the library. The renderer then loads shaders from
# memory — the binary is self-contained, no loose shaders/ dir ships. Pure
# CMake (file(READ ... HEX)); see embed_shaders.cmake and Phase 8 of
# openpete-native-renderer.md. (A dev/modding file override via
# OPENPETE_SHADER_DIR still reads loose .spv from disk — e.g. this build's
# ${RHI_SHADER_OUT_DIR} — without a relink.)
set(RHI_EMBED_C ${CMAKE_CURRENT_BINARY_DIR}/rhi_shaders_embedded.c)
string(REPLACE ";" "|" RHI_SHADER_SPV_ARG "${RHI_SHADER_SPV}")
add_custom_command(
    OUTPUT ${RHI_EMBED_C}
    COMMAND ${CMAKE_COMMAND}
            -DSPV_LIST=${RHI_SHADER_SPV_ARG}
            -DOUT_C=${RHI_EMBED_C}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_shaders.cmake
    DEPENDS ${RHI_SHADER_SPV} ${CMAKE_CURRENT_SOURCE_DIR}/embed_shaders.cmake
    COMMENT "Embedding SPIR-V shaders -> rhi_shaders_embedded.c"
    VERBATIM)

add_library(openpete_rhi STATIC
    rhi_vulkan.c
    ${RHI_EMBED_C}
    ${VOLK_DIR}/volk.c
)
target_include_directories(openpete_rhi
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${VULKAN_HEADERS_INC} ${VOLK_DIR}
)
target_compile_definitions(openpete_rhi
    PRIVATE VK_NO_PROTOTYPES)
# volk dlopens the loader; on Linux that needs libdl (CMAKE_DL_LIBS), on
# Windows it uses LoadLibrary (no extra lib).
target_link_libraries(openpete_rhi PUBLIC SDL3::SDL3-static ${CMAKE_DL_LIBS})
set_target_properties(openpete_rhi PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)
# The embedded-shader source (RHI_EMBED_C) carries its own dependency on the
# .spv via the custom command above, which in turn depends on the glslang
# outputs — so building openpete_rhi compiles shaders and regenerates the
# embed as needed. No explicit shader target/dependency required.

# Milestone 1a/1b smoke test: a standalone Vulkan window (1a cleared the
# offscreen target; 1b.1 also draws a vertex-colored quad). Verifies the
# bringup without the rest of openpete. On by default while the foundation
# is under construction; off via -DOPENPETE_BUILD_RHI_SMOKETEST=OFF.
option(OPENPETE_BUILD_RHI_SMOKETEST "Build the RHI smoke test exe" ON)
if(OPENPETE_BUILD_RHI_SMOKETEST)
    add_executable(rhi_smoketest smoketest.c)
    target_link_libraries(rhi_smoketest PRIVATE openpete_rhi SDL3::SDL3-static)
endif()
