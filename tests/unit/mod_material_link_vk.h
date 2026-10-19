/* mod_material_link_vk.h - the Vulkan leg of mod_material_link_test: a
 * combined material module's pipelines created on a headless RHI device
 * under the Khronos validation layer, with the engine's own pipeline
 * descriptions for each contract and profile. */
#ifndef MOD_MATERIAL_LINK_VK_H
#define MOD_MATERIAL_LINK_VK_H

#include <stddef.h>
#include <stdint.h>

/** @brief Create the headless device with OPENPETE_VK_VALIDATION=1 and confirm the layer loaded.
 *
 * @return 0, or 77 when no Vulkan device can be created or the validation
 *         layer did not load. */
int link_vk_open(void);

/** @brief Create a module's pipelines and count the validation errors their creation reports.
 *
 * Scene stock: the opaque, additive and stencil-mark material pipelines
 * and the sky pipeline; scene portal: the portal pipeline (set 7 block);
 * scene terrain: the retained-terrain material pipelines of vertex
 * variants 0 and 2 in colour slots 0 and 3; decal: the moby-shadow decal
 * pipeline. Each pipeline is destroyed after its create.
 * @param w The module words.
 * @param n The word count.
 * @param contract 0 scene, 1 decal.
 * @param profile 0 stock, 1 terrain, 2 portal.
 * @param made Receives the pipelines created.
 * @param errors Receives the `Validation Error` messages logged.
 * @return 0, or -1 when a create failed. */
int link_vk_pipelines(const uint32_t* w, size_t n, int contract, int profile, int* made,
                      int* errors);

/** @brief Destroy the headless device. */
void link_vk_close(void);

#endif /* MOD_MATERIAL_LINK_VK_H */
