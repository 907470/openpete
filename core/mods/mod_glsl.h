/* mod_glsl.h — the mod GLSL front end: the stage a source file names,
 * include resolution against the staged SDK, and one glslang compile of
 * one profile to SPIR-V (vulkan1.0).
 *
 * mod_shader.c registers and caches mod shaders through it at boot; the
 * mod_shader_compile test compiles every shipped mod shader through it,
 * so the test and compile-on-boot share the compiler settings, the
 * include path and the profile rule. Depends on glslang and the logger
 * only. */
#ifndef OPENPETE_MOD_GLSL_H
#define OPENPETE_MOD_GLSL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stage values mirror glslang_stage_t so handles stay meaningful when
 * the engine is built without glslang (OPENPETE_NO_GLSLANG: prebuilt
 * .spv still loads; source compiles refuse). */
#define OPENPETE_MOD_GLSL_STAGE_VERT 0   /* == GLSLANG_STAGE_VERTEX   */
#define OPENPETE_MOD_GLSL_STAGE_FRAG 4   /* == GLSLANG_STAGE_FRAGMENT */

#define OPENPETE_MOD_GLSL_SRC_MAX  (1u << 20)   /* 1 MiB file cap */
#define OPENPETE_MOD_GLSL_PATH_MAX 512

/* The define that selects the retained-terrain engine half of
 * openpete_psx_material.glsl; injected after #version. */
#define OPENPETE_MOD_GLSL_TERRAIN_PREAMBLE "#define OP_PROFILE_TERRAIN 1\n"

/* Include resolution: a name resolves in the staged SDK shader dir
 * (<sdk>/shaders) first, then in the including shader's own dir. */
typedef struct {
    char shader_dir[OPENPETE_MOD_GLSL_PATH_MAX];      /* dir of the shader */
    char sdk_shader_dir[OPENPETE_MOD_GLSL_PATH_MAX];  /* <sdk>/shaders     */
} openpete_mod_glsl_include_t;

/* ".vert" -> STAGE_VERT, ".frag" -> STAGE_FRAG, anything else -1. */
int openpete_mod_glsl_stage_from_path(const char* path);

/* Whole file, NUL-terminated, malloc'd; NULL when missing or over
 * OPENPETE_MOD_GLSL_SRC_MAX. */
char* openpete_mod_glsl_read_file(const char* path, size_t* len_out);

/* `shader_path` is the shader's file path; `sdk_dir` the SDK root. */
void openpete_mod_glsl_include_init(openpete_mod_glsl_include_t* inc,
                                    const char* shader_path,
                                    const char* sdk_dir);

/* 1 when the source is a material fragment, which the engine compiles a
 * second time under OPENPETE_MOD_GLSL_TERRAIN_PREAMBLE for the retained
 * terrain pipelines. */
int openpete_mod_glsl_has_terrain_profile(const char* src, int stage);

/* One profile of `src` -> malloc'd SPIR-V words in *out_words. Returns the
 * word count, 0 on error with the glslang log written to `log_path`.
 * `preamble` (may be NULL) is injected after #version. */
size_t openpete_mod_glsl_compile(const char* src, int stage,
                                 const openpete_mod_glsl_include_t* inc,
                                 const char* preamble, const char* log_path,
                                 uint32_t** out_words);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MOD_GLSL_H */
