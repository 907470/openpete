# core/rhi - game-agnostic Render Hardware Interface (thin) + Vulkan backend.
#
# The native renderer's foundation. Lives under core/ because it is both
# game-agnostic and platform-agnostic (windowing/surface via SDL, never a
# platform header).
#
# Vulkan is loaded dynamically via volk (third_party/volk): it dlopens
# libvulkan.so.1 / LoadLibrary("vulkan-1.dll") at runtime, so NO Vulkan
# loader import library is linked on any platform and the Windows
# cross-build needs no Vulkan SDK - only the vendored headers
# (third_party/vulkan-headers). The rhi.h seam keeps Metal/WebGPU additive.

set(VULKAN_HEADERS_INC ${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include)
set(VOLK_DIR           ${CMAKE_SOURCE_DIR}/third_party/volk)

# Shaders: GLSL -> SPIR-V via glslangValidator (a host build tool; .spv is
# portable, so this is identical for native and cross builds).
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
set(RHI_SHADER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(RHI_SHADER_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${RHI_SHADER_OUT_DIR})
set(RHI_SHADERS quad.vert quad.frag quad_key.frag psx_sample.vert psx_scene.vert psx_sample.frag psx_scene.frag psx_scene_key.frag psx_key_cutout.frag blit.frag freeze_overlay.frag resolve.frag overlay_resolve.frag shadow_decal.frag shadow_decal_tex.vert shadow_decal_tex.frag prepass.vert terrain_key.vert occl_downsample.frag hd_tex.frag hd_tex_cutout.frag postfx_fs.vert raster_quantize.frag psx_dither.frag moby_rank.vert moby_rank.frag moby_rank_cutout.frag terrain.vert terrain.frag terrain_prepass.frag terrain_key_cache.frag)
# Shared .glsl includes (GL_GOOGLE_include_directive). Every shader DEPENDS
# on all of them: the set is tiny, and a per-shader dependency scan would be
# one more thing to keep in sync with the #include lines.
set(RHI_SHADER_INCLUDES ${RHI_SHADER_SRC_DIR}/psx_clut.glsl
                        ${RHI_SHADER_SRC_DIR}/psx_key_contest.glsl
                        ${RHI_SHADER_SRC_DIR}/psx_unorm8.glsl
                        ${RHI_SHADER_SRC_DIR}/psx_viewz.glsl
                        ${RHI_SHADER_SRC_DIR}/portal_cover.glsl
                        ${RHI_SHADER_SRC_DIR}/terrain_shade.glsl)
# terrain.vert and its four variants are rewritten to SSA form after
# glslangValidator: every function-local variable whose loads and stores are
# whole-value is promoted to an SSA value (spirv-opt's local single-block and
# single-store elimination, then ssa-rewrite). glslangValidator -V emits each
# local as a Function-storage variable with explicit loads and stores. For
# the cell-record variant the NVIDIA driver compiles that memory form of this
# large fp64 stage into code whose output differs between executions of the
# same draw on the same inputs: a record's cells land at wrong positions or
# not at all for one present. The passes replace each load with the value
# last stored (a phi where control flow merges) and drop the stores; no
# arithmetic instruction is added, reordered or re-associated, and
# NoContraction stays on every arithmetic result, so the SSA form is the same
# computation, and the driver compiles it to a stage whose output is a
# function of its inputs.
find_program(SPIRV_OPT spirv-opt REQUIRED)
set(RHI_TERRAIN_SSA_ARGS --eliminate-local-single-block
                         --eliminate-local-single-store
                         --ssa-rewrite)
set(RHI_SHADER_SPV "")
foreach(s IN LISTS RHI_SHADERS)
    set(post "")
    if(s STREQUAL "terrain.vert")
        set(post COMMAND ${SPIRV_OPT} ${RHI_TERRAIN_SSA_ARGS}
                         ${RHI_SHADER_OUT_DIR}/${s}.spv -o ${RHI_SHADER_OUT_DIR}/${s}.spv)
    endif()
    add_custom_command(
        OUTPUT ${RHI_SHADER_OUT_DIR}/${s}.spv
        COMMAND ${GLSLANG_VALIDATOR} -V -I${RHI_SHADER_SRC_DIR}
                ${RHI_SHADER_SRC_DIR}/${s}
                -o ${RHI_SHADER_OUT_DIR}/${s}.spv
        ${post}
        DEPENDS ${RHI_SHADER_SRC_DIR}/${s} ${RHI_SHADER_INCLUDES}
        COMMENT "glslangValidator ${s} -> ${s}.spv"
        VERBATIM)
    list(APPEND RHI_SHADER_SPV ${RHI_SHADER_OUT_DIR}/${s}.spv)
endforeach()
# quad_key_cover.frag / psx_scene_key_cover.frag: the keyed scene fragment
# stages recompiled with the portal plane-cover discard (portal_cover.glsl),
# for the CPU-fed blended terrain pipelines; their layout adds the cover
# planes' UBO at set 5.
foreach(s IN ITEMS quad_key psx_scene_key)
    add_custom_command(
        OUTPUT ${RHI_SHADER_OUT_DIR}/${s}_cover.frag.spv
        COMMAND ${GLSLANG_VALIDATOR} -V -I${RHI_SHADER_SRC_DIR} --define-macro OP_PORTAL_COVER=1
                ${RHI_SHADER_SRC_DIR}/${s}.frag
                -o ${RHI_SHADER_OUT_DIR}/${s}_cover.frag.spv
        DEPENDS ${RHI_SHADER_SRC_DIR}/${s}.frag ${RHI_SHADER_INCLUDES}
        COMMENT "glslangValidator ${s}.frag (OP_PORTAL_COVER) -> ${s}_cover.frag.spv"
        VERBATIM)
    list(APPEND RHI_SHADER_SPV ${RHI_SHADER_OUT_DIR}/${s}_cover.frag.spv)
endforeach()
# terrain_anim.vert: terrain.vert recompiled with the poly-anim vertex-lerp
# path (second vertex binding = previous-tick positions/corners).
add_custom_command(
    OUTPUT ${RHI_SHADER_OUT_DIR}/terrain_anim.vert.spv
    COMMAND ${GLSLANG_VALIDATOR} -V -I${RHI_SHADER_SRC_DIR} --define-macro TERRAIN_ANIM=1
            ${RHI_SHADER_SRC_DIR}/terrain.vert
            -o ${RHI_SHADER_OUT_DIR}/terrain_anim.vert.spv
    COMMAND ${SPIRV_OPT} ${RHI_TERRAIN_SSA_ARGS}
            ${RHI_SHADER_OUT_DIR}/terrain_anim.vert.spv -o ${RHI_SHADER_OUT_DIR}/terrain_anim.vert.spv
    DEPENDS ${RHI_SHADER_SRC_DIR}/terrain.vert ${RHI_SHADER_INCLUDES}
    COMMENT "glslangValidator terrain.vert (TERRAIN_ANIM) -> terrain_anim.vert.spv"
    VERBATIM)
list(APPEND RHI_SHADER_SPV ${RHI_SHADER_OUT_DIR}/terrain_anim.vert.spv)
# terrain_near.vert: terrain.vert recompiled with the near-plane clip-fan path
# (second vertex binding = the emitted triangle's other two members).
add_custom_command(
    OUTPUT ${RHI_SHADER_OUT_DIR}/terrain_near.vert.spv
    COMMAND ${GLSLANG_VALIDATOR} -V -I${RHI_SHADER_SRC_DIR} --define-macro TERRAIN_NEAR=1
            ${RHI_SHADER_SRC_DIR}/terrain.vert
            -o ${RHI_SHADER_OUT_DIR}/terrain_near.vert.spv
    COMMAND ${SPIRV_OPT} ${RHI_TERRAIN_SSA_ARGS}
            ${RHI_SHADER_OUT_DIR}/terrain_near.vert.spv -o ${RHI_SHADER_OUT_DIR}/terrain_near.vert.spv
    DEPENDS ${RHI_SHADER_SRC_DIR}/terrain.vert ${RHI_SHADER_INCLUDES}
    COMMENT "glslangValidator terrain.vert (TERRAIN_NEAR) -> terrain_near.vert.spv"
    VERBATIM)
list(APPEND RHI_SHADER_SPV ${RHI_SHADER_OUT_DIR}/terrain_near.vert.spv)
# terrain_cells_near.vert / terrain_cells_anim.vert: terrain.vert recompiled
# for the 4x4 near-cell records (one instance-rate binding; each cell vertex
# derived from its record), with the near clip-fan or the poly-anim lerp.
foreach(v near anim)
    string(TOUPPER ${v} V)
    add_custom_command(
        OUTPUT ${RHI_SHADER_OUT_DIR}/terrain_cells_${v}.vert.spv
        COMMAND ${GLSLANG_VALIDATOR} -V -I${RHI_SHADER_SRC_DIR}
                --define-macro TERRAIN_CELLS=1 --define-macro TERRAIN_${V}=1
                ${RHI_SHADER_SRC_DIR}/terrain.vert
                -o ${RHI_SHADER_OUT_DIR}/terrain_cells_${v}.vert.spv
        COMMAND ${SPIRV_OPT} ${RHI_TERRAIN_SSA_ARGS}
                ${RHI_SHADER_OUT_DIR}/terrain_cells_${v}.vert.spv
                -o ${RHI_SHADER_OUT_DIR}/terrain_cells_${v}.vert.spv
        DEPENDS ${RHI_SHADER_SRC_DIR}/terrain.vert ${RHI_SHADER_INCLUDES}
        COMMENT "glslangValidator terrain.vert (TERRAIN_CELLS, TERRAIN_${V}) -> terrain_cells_${v}.vert.spv"
        VERBATIM)
    list(APPEND RHI_SHADER_SPV ${RHI_SHADER_OUT_DIR}/terrain_cells_${v}.vert.spv)
endforeach()
# Embed the compiled .spv into the binary: a build-time `cmake -P` step turns
# the SPIR-V into a generated C source (byte arrays + a name->blob table),
# which is compiled into the library. The renderer then loads shaders from
# memory - the binary is self-contained, no loose shaders/ dir ships. Pure
# CMake (file(READ ... HEX)); see embed_shaders.cmake. (A dev/modding file override via
# OPENPETE_SHADER_DIR still reads loose .spv from disk - e.g. this build's
# ${RHI_SHADER_OUT_DIR} - without a relink.)
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
    rhi_memtype.c
    rhi_gpu_audit.c
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
# outputs - so building openpete_rhi compiles shaders and regenerates the
# embed as needed. No explicit shader target/dependency required.

# RHI smoke test: a standalone Vulkan window that draws three quads sampled
# from a synthetic PSX VRAM pattern and a vertex-colored quad over a cycling
# clear color. Verifies the bringup without the rest of openpete. On by
# default; off via -DOPENPETE_BUILD_RHI_SMOKETEST=OFF.
option(OPENPETE_BUILD_RHI_SMOKETEST "Build the RHI smoke test exe" ON)
if(OPENPETE_BUILD_RHI_SMOKETEST)
    add_executable(rhi_smoketest smoketest.c)
    target_link_libraries(rhi_smoketest PRIVATE openpete_rhi SDL3::SDL3-static)
endif()
