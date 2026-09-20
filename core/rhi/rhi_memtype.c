#include "rhi_memtype.h"

int rhi_memtype_candidates(const VkPhysicalDeviceMemoryProperties *mp,
                           uint32_t type_bits, uint32_t out[], int max)
{
    if (!mp || !out || max <= 0) return 0;
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int n = 0;
    /* Pass 0 emits the DEVICE_LOCAL candidates, pass 1 the rest; within a
     * pass the order is the memory table's own index order. */
    for (int pass = 0; pass < 2 && n < max; pass++) {
        for (uint32_t i = 0; i < mp->memoryTypeCount && n < max; i++) {
            if (!(type_bits & (1u << i))) continue;
            VkMemoryPropertyFlags f = mp->memoryTypes[i].propertyFlags;
            if ((f & host) != host) continue;
            int device_local = (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
            if (device_local != (pass == 0)) continue;
            out[n++] = i;
        }
    }
    return n;
}
