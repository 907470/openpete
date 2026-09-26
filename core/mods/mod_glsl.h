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

/** @brief Map a shader path's extension to its stage.
 *
 * @param path the shader file path.
 * @return OPENPETE_MOD_GLSL_STAGE_VERT for a `.vert` suffix,
 *         OPENPETE_MOD_GLSL_STAGE_FRAG for `.frag`, -1 otherwise. */
int openpete_mod_glsl_stage_from_path(const char* path);

/** @brief Read a whole file into a NUL-terminated malloc'd buffer.
 *
 * The caller frees the buffer.
 * @param path the file path.
 * @param len_out receives the byte count read, excluding the NUL; may be
 *        NULL.
 * @return the buffer, or NULL when the file does not open, is larger than
 *         OPENPETE_MOD_GLSL_SRC_MAX, or the allocation fails. */
char* openpete_mod_glsl_read_file(const char* path, size_t* len_out);

/** @brief Fill an include context for one shader.
 *
 * shader_dir is shader_path cut at its last '/' (the whole path when it
 * holds none); sdk_shader_dir is `<sdk_dir>/shaders`. Both truncate at
 * OPENPETE_MOD_GLSL_PATH_MAX.
 * @param inc the context to fill.
 * @param shader_path the shader's file path.
 * @param sdk_dir the staged SDK root. */
void openpete_mod_glsl_include_init(openpete_mod_glsl_include_t* inc,
                                    const char* shader_path,
                                    const char* sdk_dir);

/** @brief Test whether a shader is a material fragment that also needs the terrain profile.
 *
 * True for a fragment stage whose source contains the text
 * `openpete_psx_material.glsl` anywhere. The engine compiles such a shader
 * a second time under OPENPETE_MOD_GLSL_TERRAIN_PREAMBLE for the retained
 * terrain pipelines.
 * @param src the NUL-terminated shader source.
 * @param stage the shader's stage.
 * @return 1 when the terrain profile applies, 0 otherwise. */
int openpete_mod_glsl_has_terrain_profile(const char* src, int stage);

/** @brief Compile one profile of a GLSL source to SPIR-V (vulkan1.0, SPIR-V 1.0).
 *
 * Initializes the glslang process on the first call and never finalizes
 * it. Includes resolve through `inc`: the SDK shader dir first, then the
 * shader's own dir, to a depth of 8. A preprocess, parse or link failure
 * writes the phase and the glslang info logs to log_path; a failure to
 * create the shader or an empty SPIR-V result writes no log. SPIR-V
 * generator messages go to the engine log at info level. Built with
 * OPENPETE_NO_GLSLANG, every call fails and writes a log naming the
 * prebuilt .spv route.
 * @param src the NUL-terminated shader source.
 * @param stage OPENPETE_MOD_GLSL_STAGE_VERT or OPENPETE_MOD_GLSL_STAGE_FRAG.
 * @param inc the include context; must outlive the call.
 * @param preamble text injected after #version, or NULL.
 * @param log_path the file a failure log is written to.
 * @param out_words receives the malloc'd SPIR-V words on success; untouched
 *        on failure.
 * @return the word count, or 0 on failure. */
size_t openpete_mod_glsl_compile(const char* src, int stage,
                                 const openpete_mod_glsl_include_t* inc,
                                 const char* preamble, const char* log_path,
                                 uint32_t** out_words);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MOD_GLSL_H */
