/* mod_spv_cdata.h - move large constant tables of a material fragment's
 * SPIR-V into a read-only storage buffer.
 *
 * A GLSL `const T name[N] = T[](...)` indexed at run time compiles
 * (glslang, vulkan1.0) to a Function-scope array variable that one OpStore
 * of an OpConstantComposite fills before an OpAccessChain with a
 * non-constant index reads it; a non-const global with an initializer is
 * the same shape in Private storage. Drivers lower that shape to a
 * per-invocation copy of the whole table, and some (NVIDIA) spend seconds
 * per pipeline compiling it for a table of tens of kilobytes. The rewrite
 * here replaces each such variable with one member of a storage block the
 * engine binds at OPENPETE_MOD_SPV_CDATA_SET / OPENPETE_MOD_SPV_CDATA_BINDING,
 * and returns the block's std430 bytes for the engine to upload. The
 * shader computes the same values; only where the table lives changes.
 *
 * Depends on nothing but libc; mod_shader.c runs it on every material
 * fragment profile at registration, and mod_shader_compile_test runs it on
 * every shipped mod shader. */
#ifndef OPENPETE_MOD_SPV_CDATA_H
#define OPENPETE_MOD_SPV_CDATA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The material contract set (set 3: engine block at binding 0, the mod's
 * block at binding 1) gains the storage block at binding 2. The RHI's
 * contract-set layout (rhi_vulkan.c ubo_set_layout) declares the same
 * binding; the two must agree. */
#define OPENPETE_MOD_SPV_CDATA_SET     3u
#define OPENPETE_MOD_SPV_CDATA_BINDING 2u

/* A table moves when its std430 size is at least this many bytes: 256
 * 32-bit words, more than the 255 registers one NVIDIA thread can address
 * and the 256 VGPRs of one AMD lane, so no driver can keep such a table in
 * registers and every driver must place it in memory; the storage buffer
 * is that memory without a per-invocation copy. A smaller table stays
 * where the shader declared it. */
#define OPENPETE_MOD_SPV_CDATA_MIN_BYTES 1024u

/* What a rewrite moved: the block's bytes (malloc'd; NULL with len 0 when
 * nothing moved) and how many tables went into it. */
typedef struct {
    uint8_t* bytes;
    size_t   len;
    int      tables;
} openpete_mod_spv_cdata_t;

/** @brief Move every large run-time-indexed constant table of a SPIR-V module into the engine's storage block.
 *
 * A table qualifies when it is an OpVariable of Function or Private storage
 * whose type is an array of a 32-bit int, uint or float scalar or 2-4
 * component vector; whose only write is its initializer or one OpStore of
 * an OpConstantComposite (or OpConstantNull) of that type that precedes
 * every read (a Function store in the same block as every access chain; a
 * Private store in the entry point's first block, before any call); whose
 * every other use is an OpAccessChain or OpInBoundsAccessChain with at
 * least one index, each result used only as an OpLoad pointer; at least
 * one of whose access chains has a first index that is not an OpConstant;
 * and whose std430 size is at least OPENPETE_MOD_SPV_CDATA_MIN_BYTES. An
 * id appearing anywhere else, even as a literal that happens to equal it,
 * disqualifies the table, so the rewrite never changes a module it does
 * not fully understand.
 *
 * Each qualifying table becomes one member of a NonWritable block at
 * OPENPETE_MOD_SPV_CDATA_SET / OPENPETE_MOD_SPV_CDATA_BINDING (Uniform
 * storage with BufferBlock before SPIR-V 1.3, StorageBuffer with Block
 * after), members at 16-byte aligned offsets with std430 array strides
 * (4, 8, 16, 16 for 1-4 components); tables sharing one constant share one
 * member. The variable, its store, its debug names and its decorations are
 * removed, each access chain is rebased onto the block with the member
 * index first, and a constant composite left unreferenced is removed.
 * @param in The module words, starting with the magic number.
 * @param n The word count.
 * @param out Receives the rewritten module (malloc'd) when a table moved;
 *        untouched otherwise.
 * @param out_n Receives its word count when a table moved.
 * @param data Receives the block's bytes and the table count; zeroed when
 *        nothing moved.
 * @return The number of tables moved (0 leaves the module as it was), or
 *         -1 for a malformed module or an allocation failure. */
int openpete_mod_spv_cdata_extract(const uint32_t* in, size_t n,
                                   uint32_t** out, size_t* out_n,
                                   openpete_mod_spv_cdata_t* data);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MOD_SPV_CDATA_H */
