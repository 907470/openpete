/* rhi_memtype_candidates_test - the memory-type preference order for the
 * host-written, GPU-read vertex arenas (core/rhi/rhi_memtype.c).
 *
 * The order cannot be observed on a software rasteriser: llvmpipe advertises
 * one memory type, so no run of the engine on this machine executes a second
 * attempt. The tables below are the two real ones the function is asked to
 * order: a discrete GPU with a small device-local host-visible heap beside a
 * host-visible system-memory type, and llvmpipe's single unified type. The
 * discrete case also drives a fake allocator that refuses the first
 * candidate, which is the whole point of the list. */
#include "rhi_memtype.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static void set_type(VkPhysicalDeviceMemoryProperties *mp, uint32_t i,
                     VkMemoryPropertyFlags flags, uint32_t heap)
{
    mp->memoryTypes[i].propertyFlags = flags;
    mp->memoryTypes[i].heapIndex = heap;
}

/* A discrete GPU: type 1 DEVICE_LOCAL only (the video-memory heap), type 3
 * HOST_VISIBLE|HOST_COHERENT (system memory), type 5
 * DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT (the small mapped-VRAM heap).
 * Types 0, 2, 4 carry no flags. */
static void reporter_table(VkPhysicalDeviceMemoryProperties *mp)
{
    memset(mp, 0, sizeof *mp);
    mp->memoryTypeCount = 6;
    mp->memoryHeapCount = 3;
    set_type(mp, 1, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    set_type(mp, 3, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1);
    set_type(mp, 5, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 2);
}

/* Stands in for vkAllocateMemory: every type allocates except `fail_type`. */
static int fake_alloc(uint32_t type, int fail_type)
{
    return type != (uint32_t)fail_type;
}

/* The first candidate that the fake allocator accepts, or -1. */
static int alloc_through(const uint32_t *cand, int n, int fail_type)
{
    for (int i = 0; i < n; i++)
        if (fake_alloc(cand[i], fail_type)) return (int)cand[i];
    return -1;
}

int main(void)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t out[VK_MAX_MEMORY_TYPES];

    /* Discrete GPU, buffer accepting types 0,1,3,4,5 (memoryTypeBits 0x3b):
     * the mapped-VRAM type first, system memory second. */
    reporter_table(&mp);
    int n = rhi_memtype_candidates(&mp, 0x3bu, out, VK_MAX_MEMORY_TYPES);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(out[0] == 5);
        CHECK(out[1] == 3);
    }
    /* The reported failure: the mapped-VRAM heap refuses the allocation and
     * the buffer lands in system memory instead of failing. */
    CHECK(alloc_through(out, n, 5) == 3);
    CHECK(alloc_through(out, n, -1) == 5);

    /* A buffer that accepts only the device-local-only type has no
     * host-visible candidate at all. */
    n = rhi_memtype_candidates(&mp, 1u << 1, out, VK_MAX_MEMORY_TYPES);
    CHECK(n == 0);

    /* llvmpipe: one unified type. HOST_CACHED does not order the list, and
     * the single type is still a candidate. */
    memset(&mp, 0, sizeof mp);
    mp.memoryTypeCount = 1;
    mp.memoryHeapCount = 1;
    set_type(&mp, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
    n = rhi_memtype_candidates(&mp, 0x1u, out, VK_MAX_MEMORY_TYPES);
    CHECK(n == 1);
    if (n == 1) CHECK(out[0] == 0);

    /* Host-visible but not host-coherent: the arenas are written through a
     * persistent mapping with no explicit flush, so such a type is not a
     * candidate. */
    memset(&mp, 0, sizeof mp);
    mp.memoryTypeCount = 2;
    mp.memoryHeapCount = 1;
    set_type(&mp, 0, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0);
    set_type(&mp, 1, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0);
    n = rhi_memtype_candidates(&mp, 0x3u, out, VK_MAX_MEMORY_TYPES);
    CHECK(n == 0);

    /* `max` bounds the write: a table of two candidates asked for one
     * returns the preferred one only. */
    reporter_table(&mp);
    out[1] = 0xffffffffu;
    n = rhi_memtype_candidates(&mp, 0x3bu, out, 1);
    CHECK(n == 1);
    CHECK(out[0] == 5);
    CHECK(out[1] == 0xffffffffu);

    printf("rhi_memtype_candidates_test: %s (%d failures)\n",
           g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
