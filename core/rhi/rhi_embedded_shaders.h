#ifndef RHI_EMBEDDED_SHADERS_H
#define RHI_EMBEDDED_SHADERS_H

/* SPIR-V shaders embedded in the binary so the native renderer ships as a
 * self-contained executable (no loose shaders/ dir). The table below is
 * GENERATED at build time by embed_shaders.py from the glslangValidator
 * .spv outputs; the loader (rhi_vulkan.c:load_shader_module) resolves a blob
 * by its .spv filename - the same string the pipeline descs already use.
 * A dev/modding file override is available via OPENPETE_SHADER_DIR. */

typedef struct {
    const char          *name;  /* .spv filename, e.g. "quad.vert.spv" */
    const unsigned char *data;  /* SPIR-V bytes (little-endian, as on disk) */
    unsigned             size;  /* byte count (always a multiple of 4) */
} rhi_embedded_shader;

extern const rhi_embedded_shader g_rhi_embedded_shaders[];
extern const unsigned            g_rhi_embedded_shader_count;

#endif /* RHI_EMBEDDED_SHADERS_H */
