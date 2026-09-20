/* Memory-type selection for host-written, GPU-read buffers.
 *
 * Separate from rhi_vulkan.c so the ordering can be checked against a
 * device's reported memory table without a Vulkan device: the header needs
 * only the Vulkan type definitions. */
#ifndef RHI_MEMTYPE_H
#define RHI_MEMTYPE_H

#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Write every memory-type index usable for a host-mapped, GPU-read buffer
 * into `out` (at most `max` entries) and return the count.
 *
 * A candidate is set in `type_bits` (the buffer's memoryTypeBits) and carries
 * both HOST_VISIBLE and HOST_COHERENT: the buffer is mapped for the lifetime
 * of the allocation and written without an explicit flush. DEVICE_LOCAL
 * candidates come first, in index order, then the rest in index order: a
 * device-local host-visible type keeps the vertex stage's fetch off the host
 * bus, and a heap that cannot satisfy the allocation falls through to system
 * memory, which costs bandwidth and not correctness. HOST_CACHED does not
 * order the list: the CPU only writes these buffers and never reads them
 * back, so a cached type is neither better nor worse for this use. */
int rhi_memtype_candidates(const VkPhysicalDeviceMemoryProperties *mp,
                           uint32_t type_bits, uint32_t out[], int max);

#ifdef __cplusplus
}
#endif

#endif /* RHI_MEMTYPE_H */
