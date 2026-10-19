/* mod_spv_reflect.h - read-only queries on a mod shader's SPIR-V: debug
 * names, the std140 layout of a uniform block or named struct, a member
 * array's offset and stride, and properties of one function and the
 * functions it calls (whether any terminates the invocation, whether the
 * function returns its parameter unchanged).
 *
 * Depends on nothing but libc. mod_shader.c and mod_mat_link.c run these
 * on glslang output at registration and on combined material modules; the
 * mod_material_link test runs them on every module it builds. A malformed
 * module (a zero or overlong word count) ends a walk early and reads as
 * "not found", never as a match. Lookups stop at the end of the section
 * the module's logical layout puts the sought instruction in: names and
 * decorations before the first type, globals before the first function. */
#ifndef OPENPETE_MOD_SPV_REFLECT_H
#define OPENPETE_MOD_SPV_REFLECT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Return 1 when a SPIR-V module has an OpName whose string starts with the given bytes.
 *
 * A trailing NUL included in @p len makes the match exact.
 * @param w The SPIR-V words, starting with the magic number.
 * @param n The word count.
 * @param name The bytes to match.
 * @param len Their count.
 * @return 1 on a match, 0 otherwise. */
int openpete_mod_spv_has_name(const uint32_t* w, size_t n, const char* name, size_t len);

/** @brief Read the member offsets and the size of the uniform block at a descriptor set and binding.
 *
 * The block is the pointee struct of the first OpVariable decorated with
 * both DescriptorSet @p set and Binding @p binding. The size is the
 * largest member offset plus that member's size (std140 strides from the
 * module's ArrayStride and MatrixStride decorations), not rounded.
 * @param w The SPIR-V words.
 * @param n The word count.
 * @param set The descriptor set.
 * @param binding The binding.
 * @param offs Receives up to @p max member offsets, in member order; may be NULL.
 * @param max Capacity of @p offs.
 * @param size Receives the block size; may be NULL.
 * @return The member count, or -1 when no such block exists or its layout
 *         cannot be read. */
int openpete_mod_spv_block_layout(const uint32_t* w, size_t n, uint32_t set, uint32_t binding,
                                  uint32_t* offs, int max, uint32_t* size);

/** @brief Read the member offsets and the size of the struct type whose OpName is exactly @p name.
 *
 * @param w The SPIR-V words.
 * @param n The word count.
 * @param name The struct's debug name.
 * @param offs Receives up to @p max member offsets; may be NULL.
 * @param max Capacity of @p offs.
 * @param size Receives the struct size as openpete_mod_spv_block_layout computes it; may be NULL.
 * @return The member count, or -1 when no struct carries the name or its
 *         layout cannot be read. */
int openpete_mod_spv_struct_layout(const uint32_t* w, size_t n, const char* name,
                                   uint32_t* offs, int max, uint32_t* size);

/** @brief Read the offset of a struct member whose type is an array, and that array type's ArrayStride.
 *
 * @param w The SPIR-V words.
 * @param n The word count.
 * @param name The struct's exact debug name.
 * @param member The member index.
 * @param offset Receives the member's Offset decoration.
 * @param stride Receives the array type's ArrayStride decoration.
 * @return 0, or -1 when the struct, the member, its offset or its stride is missing. */
int openpete_mod_spv_member_array(const uint32_t* w, size_t n, const char* name, uint32_t member,
                                  uint32_t* offset, uint32_t* stride);

/** @brief Test whether a function, or any function it calls directly or transitively, ends the invocation.
 *
 * The function is the one whose OpName starts with @p fn_prefix; with no
 * such name every function that is not an entry point is tested. Ending
 * the invocation is an OpKill (GLSL `discard` under SPIR-V 1.0),
 * OpTerminateInvocation or OpDemoteToHelperInvocation.
 * @param w The SPIR-V words.
 * @param n The word count.
 * @param fn_prefix The name prefix, such as "op_material(".
 * @return 1 when one does, 0 when none does, -1 for a malformed module. */
int openpete_mod_spv_fn_kills(const uint32_t* w, size_t n, const char* fn_prefix);

/** @brief Test whether a one-parameter function returns its parameter unchanged.
 *
 * True when the body of the function whose OpName starts with
 * @p fn_prefix is one block holding, besides debug-line instructions, an
 * OpLoad of the parameter and an OpReturnValue of that load (glslang
 * passes an `in` parameter by Function pointer), or an OpReturnValue of
 * the parameter itself.
 * @param w The SPIR-V words.
 * @param n The word count.
 * @param fn_prefix The name prefix, such as "op_material(".
 * @return 1 for an identity function, 0 otherwise or when no function
 *         carries the name. */
int openpete_mod_spv_fn_identity(const uint32_t* w, size_t n, const char* fn_prefix);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MOD_SPV_REFLECT_H */
