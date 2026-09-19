/* mod_glsl.c — the mod GLSL front end (see mod_glsl.h).
 *
 * Mods ship GLSL source; the engine compiles it to SPIR-V with in-process
 * glslang, the same compiler and dialect (vulkan1.0) that builds the
 * engine's own shaders, so no shader toolchain ships in any dist. */

#include "mod_glsl.h"
#include "openpete_log.h"

#ifndef OPENPETE_NO_GLSLANG
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int openpete_mod_glsl_stage_from_path(const char* path) {
    size_t n = strlen(path);
    if (n >= 5 && strcmp(path + n - 5, ".vert") == 0) return OPENPETE_MOD_GLSL_STAGE_VERT;
    if (n >= 5 && strcmp(path + n - 5, ".frag") == 0) return OPENPETE_MOD_GLSL_STAGE_FRAG;
    return -1;
}

char* openpete_mod_glsl_read_file(const char* path, size_t* len_out) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || (size_t)n > OPENPETE_MOD_GLSL_SRC_MAX) { fclose(f); return NULL; }
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (len_out) *len_out = got;
    return buf;
}

void openpete_mod_glsl_include_init(openpete_mod_glsl_include_t* inc,
                                    const char* shader_path,
                                    const char* sdk_dir) {
    snprintf(inc->shader_dir, sizeof inc->shader_dir, "%s", shader_path);
    char* slash = strrchr(inc->shader_dir, '/');
    if (slash) *slash = '\0';
    snprintf(inc->sdk_shader_dir, sizeof inc->sdk_shader_dir, "%s/shaders",
             sdk_dir);
}

int openpete_mod_glsl_has_terrain_profile(const char* src, int stage) {
    return stage == OPENPETE_MOD_GLSL_STAGE_FRAG &&
           strstr(src, "openpete_psx_material.glsl") != NULL;
}

static void write_log(const char* log_path, const char* phase,
                      const char* a, const char* b) {
    FILE* f = fopen(log_path, "wb");
    if (!f) return;
    fprintf(f, "[%s]\n%s\n%s\n", phase, a ? a : "", b ? b : "");
    fclose(f);
}

#ifdef OPENPETE_NO_GLSLANG
size_t openpete_mod_glsl_compile(const char* src, int stage,
                                 const openpete_mod_glsl_include_t* inc,
                                 const char* preamble, const char* log_path,
                                 uint32_t** out_words) {
    (void)src; (void)stage; (void)inc; (void)preamble; (void)out_words;
    write_log(log_path, "compile",
              "engine built without glslang (OPENPETE_BUILD_GLSLANG=OFF); "
              "ship a prebuilt .spv + .spv.ihash sidecar instead", NULL);
    return 0;
}
#else
static glsl_include_result_t* include_load(const char* dir, const char* name) {
    char p[OPENPETE_MOD_GLSL_PATH_MAX * 2];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    size_t len = 0;
    char* data = openpete_mod_glsl_read_file(p, &len);
    if (!data) return NULL;
    glsl_include_result_t* r = (glsl_include_result_t*)calloc(1, sizeof *r);
    if (!r) { free(data); return NULL; }
    r->header_name = strdup(name);
    r->header_data = data;
    r->header_length = len;
    return r;
}

static glsl_include_result_t* include_resolve(void* ctx_v, const char* name,
                                              const char* includer_name,
                                              size_t depth) {
    const openpete_mod_glsl_include_t* inc =
        (const openpete_mod_glsl_include_t*)ctx_v;
    (void)includer_name;
    if (depth > 8) return NULL;                       /* cycle guard */
    glsl_include_result_t* r = include_load(inc->sdk_shader_dir, name);
    if (!r) r = include_load(inc->shader_dir, name);
    return r;
}

static glsl_include_result_t* include_system(void* ctx, const char* name,
                                             const char* inc, size_t depth) {
    return include_resolve(ctx, name, inc, depth);
}
static glsl_include_result_t* include_local(void* ctx, const char* name,
                                            const char* inc, size_t depth) {
    return include_resolve(ctx, name, inc, depth);
}
static int include_free(void* ctx, glsl_include_result_t* r) {
    (void)ctx;
    if (r) {
        free((void*)r->header_name);
        free((void*)r->header_data);
        free(r);
    }
    return 0;
}

size_t openpete_mod_glsl_compile(const char* src, int stage,
                                 const openpete_mod_glsl_include_t* inc,
                                 const char* preamble, const char* log_path,
                                 uint32_t** out_words) {
    static int s_glslang_up = 0;
    if (!s_glslang_up) {
        glslang_initialize_process();  /* process-lifetime; never finalized */
        s_glslang_up = 1;
    }

    glslang_input_t input;
    memset(&input, 0, sizeof input);
    input.language = GLSLANG_SOURCE_GLSL;
    input.stage = (glslang_stage_t)stage;
    input.client = GLSLANG_CLIENT_VULKAN;
    /* vulkan1.0 — matches the engine's own `glslangValidator -V` builds */
    input.client_version = GLSLANG_TARGET_VULKAN_1_0;
    input.target_language = GLSLANG_TARGET_SPV;
    input.target_language_version = GLSLANG_TARGET_SPV_1_0;
    input.code = src;
    input.default_version = 450;
    input.default_profile = GLSLANG_NO_PROFILE;
    input.messages = GLSLANG_MSG_DEFAULT_BIT;
    input.resource = glslang_default_resource();
    input.callbacks.include_system = include_system;
    input.callbacks.include_local = include_local;
    input.callbacks.free_include_result = include_free;
    input.callbacks_ctx = (void*)inc;

    glslang_shader_t* sh = glslang_shader_create(&input);
    if (!sh) return 0;
    /* Injected after #version, so a profile define reaches the contract
     * include without the mod's source knowing which profile it is in. */
    if (preamble) glslang_shader_set_preamble(sh, preamble);
    if (!glslang_shader_preprocess(sh, &input) ||
        !glslang_shader_parse(sh, &input)) {
        write_log(log_path, "parse", glslang_shader_get_info_log(sh),
                  glslang_shader_get_info_debug_log(sh));
        glslang_shader_delete(sh);
        return 0;
    }
    glslang_program_t* prog = glslang_program_create();
    glslang_program_add_shader(prog, sh);
    if (!glslang_program_link(prog, GLSLANG_MSG_SPV_RULES_BIT |
                                    GLSLANG_MSG_VULKAN_RULES_BIT)) {
        write_log(log_path, "link", glslang_program_get_info_log(prog),
                  glslang_program_get_info_debug_log(prog));
        glslang_program_delete(prog);
        glslang_shader_delete(sh);
        return 0;
    }
    glslang_program_SPIRV_generate(prog, (glslang_stage_t)stage);
    size_t words = glslang_program_SPIRV_get_size(prog);
    uint32_t* buf = words ? (uint32_t*)malloc(words * sizeof(uint32_t)) : NULL;
    if (buf) glslang_program_SPIRV_get(prog, buf);
    const char* spv_msg = glslang_program_SPIRV_get_messages(prog);
    if (spv_msg && *spv_msg)
        OPL_INFO_F("mods", "shader: glslang: %s", spv_msg);
    glslang_program_delete(prog);
    glslang_shader_delete(sh);
    if (!buf) return 0;
    *out_words = buf;
    return words;
}
#endif /* OPENPETE_NO_GLSLANG */
