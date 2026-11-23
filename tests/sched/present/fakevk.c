/* fakevk.c - the fake Vulkan device; see fakevk.h.
 *
 * Handles of fences, semaphores, images, swapchains and command buffers are
 * pointers to the device's own records. Outside a scheduled run (rhi_create
 * in the explorer before the first fork) every call runs straight through
 * and the GPU completes each submission at once. */
#include "volk.h"
#include "fakevk.h"
#include "op_sched.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define X(n) PFN_##n n;
#include "vk_list.h"
#undef X

#define X(n) static unsigned long reach_##n;
#include "vk_list.h"
#undef X
#define R(n) (reach_##n++)

/** @brief Abort naming a Vulkan entry point the device does not model.
 * @param n Its name. */
static void trap(const char *n)
{
    fprintf(stderr, "fakevk TRAP: %s not modelled\n", n);
    abort();
}
#define X(n) static void trap_##n(void) { trap(#n); }
#include "vk_list.h"
#undef X

static uint64_t s_next = 0x1000;
/** @brief Return a fresh non-dispatchable handle value.
 * @return The value. */
static uint64_t nh(void) { return s_next += 0x10; }
static struct { int x; } g_inst, g_phys, g_dev, g_queue;
#define NH(T) ((T)(uintptr_t)nh())
#define RA __builtin_return_address(0)

/* ---- instance and physical device ---- */
static VkResult f_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *o)
{ (void)ci; (void)a; R(vkCreateInstance); *o = (VkInstance)&g_inst; return VK_SUCCESS; }
static void f_DestroyInstance(VkInstance i, const VkAllocationCallbacks *a) { (void)i; (void)a; R(vkDestroyInstance); }
static VkResult f_EnumerateInstanceLayerProperties(uint32_t *n, VkLayerProperties *p) { (void)p; R(vkEnumerateInstanceLayerProperties); *n = 0; return VK_SUCCESS; }
static PFN_vkVoidFunction f_GetInstanceProcAddr(VkInstance i, const char *n) { (void)i; (void)n; R(vkGetInstanceProcAddr); return NULL; }
static VkResult f_EnumeratePhysicalDevices(VkInstance i, uint32_t *n, VkPhysicalDevice *p)
{ (void)i; R(vkEnumeratePhysicalDevices); if (p && *n) p[0] = (VkPhysicalDevice)&g_phys; *n = 1; return VK_SUCCESS; }
static void fill_props(VkPhysicalDeviceProperties *p)
{
    memset(p, 0, sizeof *p);
    p->apiVersion = VK_API_VERSION_1_3; p->driverVersion = 1; p->vendorID = 0x10de; p->deviceID = 1;
    p->deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    snprintf(p->deviceName, sizeof p->deviceName, "fakevk");
    VkPhysicalDeviceLimits *l = &p->limits;
    l->maxImageDimension2D = 16384; l->maxPushConstantsSize = 256; l->timestampPeriod = 1.0f;
    l->timestampComputeAndGraphics = VK_TRUE; l->nonCoherentAtomSize = 64;
    l->minUniformBufferOffsetAlignment = 256; l->minStorageBufferOffsetAlignment = 64;
    l->maxBoundDescriptorSets = 8; l->maxColorAttachments = 8; l->maxSamplerAnisotropy = 16.0f;
    l->maxPerStageDescriptorSamplers = 1u << 20; l->maxPerStageDescriptorSampledImages = 1u << 20;
    l->maxDescriptorSetSampledImages = 1u << 20; l->maxStorageBufferRange = 1u << 30;
    l->maxUniformBufferRange = 65536; l->maxMemoryAllocationCount = 1u << 20;
    l->framebufferColorSampleCounts = 0x7f; l->framebufferDepthSampleCounts = 0x7f;
    l->maxFramebufferWidth = 16384; l->maxFramebufferHeight = 16384; l->maxViewports = 16;
    l->maxComputeWorkGroupCount[0] = l->maxComputeWorkGroupCount[1] = l->maxComputeWorkGroupCount[2] = 65535;
    l->maxComputeWorkGroupInvocations = 1024;
    l->maxComputeWorkGroupSize[0] = l->maxComputeWorkGroupSize[1] = 1024; l->maxComputeWorkGroupSize[2] = 64;
}
static void f_GetPhysicalDeviceProperties(VkPhysicalDevice d, VkPhysicalDeviceProperties *p) { (void)d; R(vkGetPhysicalDeviceProperties); fill_props(p); }
static void f_GetPhysicalDeviceProperties2(VkPhysicalDevice d, VkPhysicalDeviceProperties2 *p)
{
    (void)d; R(vkGetPhysicalDeviceProperties2); fill_props(&p->properties);
    for (VkBaseOutStructure *s = (VkBaseOutStructure *)p->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES) {
            VkPhysicalDeviceDriverProperties *dp = (VkPhysicalDeviceDriverProperties *)s;
            snprintf(dp->driverName, sizeof dp->driverName, "fakevk"); snprintf(dp->driverInfo, sizeof dp->driverInfo, "spike");
        }
    }
}
static void f_GetPhysicalDeviceFeatures(VkPhysicalDevice d, VkPhysicalDeviceFeatures *f)
{ (void)d; R(vkGetPhysicalDeviceFeatures); VkBool32 *b = (VkBool32 *)f; for (size_t i = 0; i < sizeof *f / sizeof *b; i++) b[i] = VK_TRUE; }
/* Every chained feature structure: every VkBool32 after the header set true. */
static size_t feature_size(VkStructureType t)
{
    switch (t) {
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR: return sizeof(VkPhysicalDevicePresentIdFeaturesKHR);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR: return sizeof(VkPhysicalDevicePresentWaitFeaturesKHR);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: return sizeof(VkPhysicalDeviceVulkan11Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: return sizeof(VkPhysicalDeviceVulkan12Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: return sizeof(VkPhysicalDeviceVulkan13Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES: return sizeof(VkPhysicalDeviceDescriptorIndexingFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES: return sizeof(VkPhysicalDeviceTimelineSemaphoreFeatures);
    default: return 0;
    }
}
static void f_GetPhysicalDeviceFeatures2(VkPhysicalDevice d, VkPhysicalDeviceFeatures2 *f)
{
    (void)d; R(vkGetPhysicalDeviceFeatures2);
    VkBool32 *b = (VkBool32 *)&f->features; for (size_t i = 0; i < sizeof f->features / sizeof *b; i++) b[i] = VK_TRUE;
    for (VkBaseOutStructure *s = (VkBaseOutStructure *)f->pNext; s; s = s->pNext) {
        const size_t sz = feature_size(s->sType);
        if (!sz) { fprintf(stderr, "fakevk: features2 chain sType %d not modelled (left zero)\n", (int)s->sType); continue; }
        VkBool32 *w = (VkBool32 *)((char *)s + sizeof(VkBaseOutStructure));
        for (size_t i = 0; i < (sz - sizeof(VkBaseOutStructure)) / sizeof *w; i++) w[i] = VK_TRUE;
    }
}
static void f_GetPhysicalDeviceFormatProperties(VkPhysicalDevice d, VkFormat fm, VkFormatProperties *p)
{ (void)d; (void)fm; R(vkGetPhysicalDeviceFormatProperties); p->linearTilingFeatures = p->optimalTilingFeatures = p->bufferFeatures = 0xffffffffu; }
static void f_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice d, VkPhysicalDeviceMemoryProperties *p)
{
    (void)d; R(vkGetPhysicalDeviceMemoryProperties); memset(p, 0, sizeof *p);
    p->memoryHeapCount = 2;
    p->memoryHeaps[0] = (VkMemoryHeap){ 8ull << 30, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT };
    p->memoryHeaps[1] = (VkMemoryHeap){ 16ull << 30, 0 };
    p->memoryTypeCount = 3;
    p->memoryTypes[0] = (VkMemoryType){ VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0 };
    p->memoryTypes[1] = (VkMemoryType){ VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 1 };
    p->memoryTypes[2] = (VkMemoryType){ VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0 };
}
static void f_GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice d, uint32_t *n, VkQueueFamilyProperties *p)
{
    (void)d; R(vkGetPhysicalDeviceQueueFamilyProperties);
    if (p && *n) p[0] = (VkQueueFamilyProperties){ VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, 1, 64, { 1, 1, 1 } };
    *n = 1;
}
static const char *const k_dev_ext[] = { "VK_KHR_swapchain", "VK_KHR_present_id", "VK_KHR_present_wait" };
static VkResult f_EnumerateDeviceExtensionProperties(VkPhysicalDevice d, const char *l, uint32_t *n, VkExtensionProperties *p)
{
    (void)d; (void)l; R(vkEnumerateDeviceExtensionProperties);
    const uint32_t k = 3;
    if (p) for (uint32_t i = 0; i < k && i < *n; i++) { memset(&p[i], 0, sizeof p[i]); snprintf(p[i].extensionName, sizeof p[i].extensionName, "%s", k_dev_ext[i]); p[i].specVersion = 1; }
    if (!p || *n >= k) *n = k;
    return VK_SUCCESS;
}
static VkResult f_EnumerateDeviceLayerProperties(VkPhysicalDevice d, uint32_t *n, VkLayerProperties *p) { (void)d; (void)p; R(vkEnumerateDeviceLayerProperties); *n = 0; return VK_SUCCESS; }
static VkResult f_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice d, uint32_t q, VkSurfaceKHR s, VkBool32 *o) { (void)d; (void)q; (void)s; R(vkGetPhysicalDeviceSurfaceSupportKHR); *o = VK_TRUE; return VK_SUCCESS; }
static VkResult f_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice d, VkSurfaceKHR s, uint32_t *n, VkSurfaceFormatKHR *f)
{ (void)d; (void)s; R(vkGetPhysicalDeviceSurfaceFormatsKHR); if (f && *n) f[0] = (VkSurfaceFormatKHR){ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR }; *n = 1; return VK_SUCCESS; }
static VkResult f_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice d, VkSurfaceKHR s, uint32_t *n, VkPresentModeKHR *m)
{
    (void)d; (void)s; R(vkGetPhysicalDeviceSurfacePresentModesKHR);
    static const VkPresentModeKHR k[] = { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR };
    if (m) for (uint32_t i = 0; i < 3 && i < *n; i++) m[i] = k[i];
    if (!m || *n >= 3) *n = 3;
    return VK_SUCCESS;
}
static void f_DestroySurfaceKHR(VkInstance i, VkSurfaceKHR s, const VkAllocationCallbacks *a) { (void)i; (void)s; (void)a; R(vkDestroySurfaceKHR); }

/* ---- device ---- */
static VkResult f_CreateDevice(VkPhysicalDevice p, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *o)
{ (void)p; (void)ci; (void)a; R(vkCreateDevice); *o = (VkDevice)&g_dev; return VK_SUCCESS; }
static void f_DestroyDevice(VkDevice d, const VkAllocationCallbacks *a) { (void)d; (void)a; R(vkDestroyDevice); }
static void f_GetDeviceQueue(VkDevice d, uint32_t f, uint32_t i, VkQueue *q) { (void)d; (void)f; (void)i; R(vkGetDeviceQueue); *q = (VkQueue)&g_queue; }

/* Generic create/destroy of non-dispatchable handles. */
#define CREATE(fn, CI, T) \
    static VkResult f_##fn(VkDevice d, const CI *ci, const VkAllocationCallbacks *a, T *o) \
    { (void)d; (void)ci; (void)a; R(vk##fn); *o = NH(T); return VK_SUCCESS; }
#define DESTROY(fn, T) \
    static void f_##fn(VkDevice d, T h, const VkAllocationCallbacks *a) { (void)d; (void)h; (void)a; R(vk##fn); }
CREATE(CreateBuffer, VkBufferCreateInfo, VkBuffer)               DESTROY(DestroyBuffer, VkBuffer)
CREATE(CreateImageView, VkImageViewCreateInfo, VkImageView)      DESTROY(DestroyImageView, VkImageView)
CREATE(CreateCommandPool, VkCommandPoolCreateInfo, VkCommandPool) DESTROY(DestroyCommandPool, VkCommandPool)
CREATE(CreateDescriptorPool, VkDescriptorPoolCreateInfo, VkDescriptorPool) DESTROY(DestroyDescriptorPool, VkDescriptorPool)
CREATE(CreateDescriptorSetLayout, VkDescriptorSetLayoutCreateInfo, VkDescriptorSetLayout) DESTROY(DestroyDescriptorSetLayout, VkDescriptorSetLayout)
CREATE(CreateFramebuffer, VkFramebufferCreateInfo, VkFramebuffer) DESTROY(DestroyFramebuffer, VkFramebuffer)
CREATE(CreatePipelineCache, VkPipelineCacheCreateInfo, VkPipelineCache) DESTROY(DestroyPipelineCache, VkPipelineCache)
CREATE(CreatePipelineLayout, VkPipelineLayoutCreateInfo, VkPipelineLayout) DESTROY(DestroyPipelineLayout, VkPipelineLayout)
CREATE(CreateQueryPool, VkQueryPoolCreateInfo, VkQueryPool)      DESTROY(DestroyQueryPool, VkQueryPool)
CREATE(CreateRenderPass, VkRenderPassCreateInfo, VkRenderPass)   DESTROY(DestroyRenderPass, VkRenderPass)
CREATE(CreateSampler, VkSamplerCreateInfo, VkSampler)            DESTROY(DestroySampler, VkSampler)
CREATE(CreateShaderModule, VkShaderModuleCreateInfo, VkShaderModule) DESTROY(DestroyShaderModule, VkShaderModule)
DESTROY(DestroyPipeline, VkPipeline)
/* Memory: host backing allocated at map. */
typedef struct fmem { VkDeviceSize size; void *host; } fmem;
static VkResult f_AllocateMemory(VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *o)
{ (void)d; (void)a; R(vkAllocateMemory); fmem *m = calloc(1, sizeof *m); m->size = ai->allocationSize; *o = (VkDeviceMemory)(uintptr_t)m; return VK_SUCCESS; }
static void f_FreeMemory(VkDevice d, VkDeviceMemory mem, const VkAllocationCallbacks *a)
{ (void)d; (void)a; R(vkFreeMemory); fmem *m = (fmem *)(uintptr_t)mem; if (m) { free(m->host); free(m); } }
static VkResult f_MapMemory(VkDevice d, VkDeviceMemory mem, VkDeviceSize off, VkDeviceSize sz, VkMemoryMapFlags fl, void **pp)
{ (void)d; (void)sz; (void)fl; R(vkMapMemory); fmem *m = (fmem *)(uintptr_t)mem; if (!m->host) m->host = calloc(1, (size_t)m->size); *pp = (char *)m->host + off; return m->host ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY; }
static void f_UnmapMemory(VkDevice d, VkDeviceMemory mem) { (void)d; (void)mem; R(vkUnmapMemory); }
static VkResult f_BindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize o) { (void)d; (void)b; (void)m; (void)o; R(vkBindBufferMemory); return VK_SUCCESS; }
static VkResult f_BindImageMemory(VkDevice d, VkImage b, VkDeviceMemory m, VkDeviceSize o) { (void)d; (void)b; (void)m; (void)o; R(vkBindImageMemory); return VK_SUCCESS; }
static void f_GetBufferMemoryRequirements(VkDevice d, VkBuffer b, VkMemoryRequirements *r)
{ (void)d; (void)b; R(vkGetBufferMemoryRequirements); r->size = 1 << 16; r->alignment = 256; r->memoryTypeBits = 0x7; }
/* Descriptor sets and pipelines. */
static VkResult f_AllocateDescriptorSets(VkDevice d, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *o)
{ (void)d; R(vkAllocateDescriptorSets); for (uint32_t i = 0; i < ai->descriptorSetCount; i++) o[i] = NH(VkDescriptorSet); return VK_SUCCESS; }
static VkResult f_FreeDescriptorSets(VkDevice d, VkDescriptorPool p, uint32_t n, const VkDescriptorSet *s) { (void)d; (void)p; (void)n; (void)s; R(vkFreeDescriptorSets); return VK_SUCCESS; }
static void f_UpdateDescriptorSets(VkDevice d, uint32_t n, const VkWriteDescriptorSet *w, uint32_t m, const VkCopyDescriptorSet *c) { (void)d; (void)n; (void)w; (void)m; (void)c; R(vkUpdateDescriptorSets); }
static VkResult f_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *o)
{ (void)d; (void)c; (void)ci; (void)a; R(vkCreateGraphicsPipelines); for (uint32_t i = 0; i < n; i++) o[i] = NH(VkPipeline); return VK_SUCCESS; }
static VkResult f_CreateComputePipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *o)
{ (void)d; (void)c; (void)ci; (void)a; R(vkCreateComputePipelines); for (uint32_t i = 0; i < n; i++) o[i] = NH(VkPipeline); return VK_SUCCESS; }
static VkResult f_GetPipelineCacheData(VkDevice d, VkPipelineCache c, size_t *n, void *p) { (void)d; (void)c; (void)p; R(vkGetPipelineCacheData); *n = 0; return VK_SUCCESS; }
static VkResult f_GetQueryPoolResults(VkDevice d, VkQueryPool q, uint32_t f, uint32_t n, size_t sz, void *p, VkDeviceSize st, VkQueryResultFlags fl)
{ (void)d; (void)q; (void)f; (void)n; (void)st; (void)fl; R(vkGetQueryPoolResults); memset(p, 0, sz); return VK_SUCCESS; }

/* Commands: no-ops. */
static void f_CmdBeginRenderPass(VkCommandBuffer c, const VkRenderPassBeginInfo *b, VkSubpassContents s) { (void)c; (void)b; (void)s; R(vkCmdBeginRenderPass); }
static void f_CmdEndRenderPass(VkCommandBuffer c) { (void)c; R(vkCmdEndRenderPass); }
static void f_CmdBindDescriptorSets(VkCommandBuffer c, VkPipelineBindPoint b, VkPipelineLayout l, uint32_t f, uint32_t n, const VkDescriptorSet *s, uint32_t dn, const uint32_t *d) { (void)c; (void)b; (void)l; (void)f; (void)n; (void)s; (void)dn; (void)d; R(vkCmdBindDescriptorSets); }
static void f_CmdBindPipeline(VkCommandBuffer c, VkPipelineBindPoint b, VkPipeline p) { (void)c; (void)b; (void)p; R(vkCmdBindPipeline); }
static void f_CmdBindVertexBuffers(VkCommandBuffer c, uint32_t f, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) { (void)c; (void)f; (void)n; (void)b; (void)o; R(vkCmdBindVertexBuffers); }
static void f_CmdClearAttachments(VkCommandBuffer c, uint32_t n, const VkClearAttachment *a, uint32_t m, const VkClearRect *r) { (void)c; (void)n; (void)a; (void)m; (void)r; R(vkCmdClearAttachments); }
static void f_CmdClearColorImage(VkCommandBuffer c, VkImage i, VkImageLayout l, const VkClearColorValue *v, uint32_t n, const VkImageSubresourceRange *r) { (void)c; (void)i; (void)l; (void)v; (void)n; (void)r; R(vkCmdClearColorImage); }
static void f_CmdCopyBuffer(VkCommandBuffer c, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *r) { (void)c; (void)s; (void)d; (void)n; (void)r; R(vkCmdCopyBuffer); }
static void f_CmdCopyBufferToImage(VkCommandBuffer c, VkBuffer s, VkImage d, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r) { (void)c; (void)s; (void)d; (void)l; (void)n; (void)r; R(vkCmdCopyBufferToImage); }
static void f_CmdCopyImageToBuffer(VkCommandBuffer c, VkImage s, VkImageLayout l, VkBuffer d, uint32_t n, const VkBufferImageCopy *r) { (void)c; (void)s; (void)d; (void)l; (void)n; (void)r; R(vkCmdCopyImageToBuffer); }
static void f_CmdDispatch(VkCommandBuffer c, uint32_t x, uint32_t y, uint32_t z) { (void)c; (void)x; (void)y; (void)z; R(vkCmdDispatch); }
static void f_CmdDraw(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t d, uint32_t e) { (void)c; (void)a; (void)b; (void)d; (void)e; R(vkCmdDraw); }
static void f_CmdFillBuffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkDeviceSize s, uint32_t d) { (void)c; (void)b; (void)o; (void)s; (void)d; R(vkCmdFillBuffer); }
static void f_CmdPipelineBarrier(VkCommandBuffer c, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f, uint32_t a, const VkMemoryBarrier *b, uint32_t e, const VkBufferMemoryBarrier *g, uint32_t h, const VkImageMemoryBarrier *i)
{ (void)c; (void)s; (void)d; (void)f; (void)a; (void)b; (void)e; (void)g; (void)h; (void)i; R(vkCmdPipelineBarrier); }
static void f_CmdPushConstants(VkCommandBuffer c, VkPipelineLayout l, VkShaderStageFlags s, uint32_t o, uint32_t n, const void *v) { (void)c; (void)l; (void)s; (void)o; (void)n; (void)v; R(vkCmdPushConstants); }
static void f_CmdResetQueryPool(VkCommandBuffer c, VkQueryPool q, uint32_t f, uint32_t n) { (void)c; (void)q; (void)f; (void)n; R(vkCmdResetQueryPool); }
static void f_CmdSetDepthBias(VkCommandBuffer c, float a, float b, float d) { (void)c; (void)a; (void)b; (void)d; R(vkCmdSetDepthBias); }
static void f_CmdSetScissor(VkCommandBuffer c, uint32_t f, uint32_t n, const VkRect2D *r) { (void)c; (void)f; (void)n; (void)r; R(vkCmdSetScissor); }
static void f_CmdSetViewport(VkCommandBuffer c, uint32_t f, uint32_t n, const VkViewport *v) { (void)c; (void)f; (void)n; (void)v; R(vkCmdSetViewport); }
static void f_CmdWriteTimestamp(VkCommandBuffer c, VkPipelineStageFlagBits s, VkQueryPool q, uint32_t i) { (void)c; (void)s; (void)q; (void)i; R(vkCmdWriteTimestamp); }

/* ---- the device model ---- */

enum { FK_MAX_THREADS = 32, FK_SC_MAX = 8, FK_CB_BLITS = 8, FK_SERIALS = 4096 };

typedef struct fk_sub fk_sub;
/** A fence: signalled, or with the submission that will signal it pending. */
typedef struct fk_fence { int signaled; fk_sub *pending; int destroyed; } fk_fence;
/** A binary semaphore: signalled, or with the submission that will signal it pending. */
typedef struct fk_sem { int signaled; fk_sub *pending; int destroyed; } fk_sem;
/** Swapchain image states. */
enum { IMG_AVAIL, IMG_ACQUIRED, IMG_QUEUED, IMG_SHOWN };
struct fk_sc;
/** An image: what it holds, and for a swapchain image its presentation state. */
typedef struct fk_image {
    uint32_t content;   /* scene id or inline tag; 0 = none */
    int assigned;       /* content was written by a blit (else the image is a render target) */
    int fresh;          /* written since its acquire (swapchain images) */
    int state;          /* IMG_* (swapchain images) */
    int acquirer;       /* thread that acquired it */
    struct fk_sc *sc;   /* owning swapchain, or NULL */
} fk_image;
/** A swapchain. */
typedef struct fk_sc {
    fk_image img[FK_SC_MAX];
    uint32_t n;
    int retired, destroyed;
    uint64_t flipped_id;     /* newest present id on screen */
    uint64_t presented_id;   /* newest present id queued */
    int64_t last_flip_at;    /* the latest flip scheduled */
    int shown;               /* image on screen, or -1 */
    int inside;              /* thread inside a call on it, or -1 */
} fk_sc;
/** One recorded blit. */
typedef struct fk_blit { fk_image *src, *dst; uint32_t val; int from_src; } fk_blit;
/** A command buffer: the blits it records. */
typedef struct fk_cb { fk_blit blit[FK_CB_BLITS]; int nblit; } fk_cb;
/** A queue submission. */
struct fk_sub { uint64_t seq; int64_t done_at; int complete; fk_fence *fence; fk_sem *sig[4]; int nsig; };
/** A scheduled flip. */
typedef struct fk_flip { fk_sc *sc; uint32_t idx; uint64_t id; } fk_flip;

static fakevk_domain s_dom = { .gpu_ns = { 200000 }, .n_gpu = 1, .vblank_ns = 2083333 };
static fakevk_hooks s_hooks;
static int s_win_w = 1280, s_win_h = 720;
static int s_lost;
static int s_queue_inside = -1;
static uint64_t s_sub_seq;
static int64_t s_last_done;
static fk_sub *s_last_sub;
static uint64_t s_last_sub_at_render;
static fk_sub *s_serial_sub[FK_SERIALS];
static uint64_t s_serial_of[FK_SERIALS];
static uint32_t s_render, s_inline_tag = 0x80000000u;
static int64_t s_call_ns[FK_MAX_THREADS];
static fk_sc *s_cur_sc;
static SDL_EventFilter s_watch_fn;
static void *s_watch_ud;
static const void *s_changed_addr;
static unsigned long s_acq_refusals;
enum { FK_FORCE_MAX = 16 };
static struct { int kind, ordinal, result; } s_force[FK_FORCE_MAX];
static int s_nforce;
static int s_call_ordinal[FK_CALL_KINDS];

unsigned long fakevk_acquire_refusals(void) { return s_acq_refusals; }

void fakevk_force(int kind, int ordinal, int result)
{
    if (s_nforce < FK_FORCE_MAX) { s_force[s_nforce].kind = kind; s_force[s_nforce].ordinal = ordinal; s_force[s_nforce].result = result; s_nforce++; }
}

void fakevk_set_domain(const fakevk_domain *d)
{
    s_dom = *d;
    if (s_dom.n_gpu < 1) { s_dom.n_gpu = 1; s_dom.gpu_ns[0] = 200000; }
    if (s_dom.vblank_ns <= 0) s_dom.vblank_ns = 2083333;
}
void fakevk_set_hooks(const fakevk_hooks *h) { s_hooks = *h; }
void fakevk_set_window(int w, int h) { s_win_w = w; s_win_h = h; }
void fakevk_window(int *w, int *h) { *w = s_win_w; *h = s_win_h; }
void fakevk_set_render(uint32_t content) { s_render = content; s_last_sub_at_render = s_sub_seq; }
void fakevk_set_inline_tag(uint32_t tag) { s_inline_tag = tag; }
uint32_t fakevk_inline_tag(void) { return s_inline_tag; }

void fakevk_frame_done(uint64_t serial)
{
    if (!s_last_sub || s_last_sub->seq <= s_last_sub_at_render) return;
    s_serial_sub[serial % FK_SERIALS] = s_last_sub;
    s_serial_of[serial % FK_SERIALS] = serial;
}

int fakevk_serial_complete(uint64_t serial)
{
    const fk_sub *sb = s_serial_sub[serial % FK_SERIALS];
    if (!sb || s_serial_of[serial % FK_SERIALS] != serial) return -1;
    return sb->complete;
}

int fakevk_acquired_outstanding(void)
{
    int n = 0;
    if (s_cur_sc && !s_cur_sc->destroyed)
        for (uint32_t i = 0; i < s_cur_sc->n; i++) n += s_cur_sc->img[i].state == IMG_ACQUIRED;
    return n;
}

int fakevk_presentable(void) { return !s_lost && s_win_w > 0 && s_win_h > 0; }
int fakevk_lost(void) { return s_lost; }

int64_t fakevk_thread_call_ns(int thread)
{
    return thread >= 0 && thread < FK_MAX_THREADS ? s_call_ns[thread] : 0;
}

/** A modelled call in progress: the host-synchronised object it holds and its start. */
typedef struct fk_call { int *inside; int64_t t0; int me; } fk_call;

/** @brief Enter a modelled call: run the harness hook, check and take host synchronisation of obj, and yield.
 * @param c The call.
 * @param name Entry point name.
 * @param inside The object's inside marker, or NULL when the call is not externally synchronised.
 * @param inside2 A second marker (the swapchain of a present), or NULL.
 * @param obj The object, for the harness hook; the trace names the entry point.
 * @param ra The RHI's call site. */
static void fk_begin(fk_call *c, const char *name, int *inside, int *inside2, const void *obj, const void *ra)
{
    c->inside = NULL;
    c->me = sched_self();
    c->t0 = sched_now();
    if (!sched_active()) return;
    if (s_hooks.call) s_hooks.call(name, obj);
    int *marks[2] = { inside, inside2 };
    for (int k = 0; k < 2; k++) {
        if (!marks[k]) continue;
        if (*marks[k] >= 0 && *marks[k] != c->me)
            sched_fail("P-OWNER: host synchronisation: %s on T%d while T%d is inside a call on the same object",
                       name, c->me, *marks[k]);
    }
    if (inside) *inside = c->me;
    if (inside2) *inside2 = c->me;
    c->inside = inside;
    sched_name(name, name);
    sched_point_ra(SCHED_OP_DEVICE, name, ra);
    c->t0 = sched_now();
    (void)inside2;
}

/** @brief Leave a modelled call: release host synchronisation and charge its virtual time to the thread.
 * @param c The call.
 * @param inside2 The second marker passed to fk_begin, or NULL. */
static void fk_end(fk_call *c, int *inside2)
{
    if (!sched_active()) return;
    if (c->me >= 0 && c->me < FK_MAX_THREADS) s_call_ns[c->me] += sched_now() - c->t0;
    if (c->inside) *c->inside = -1;
    if (inside2) *inside2 = -1;
}

/** @brief Choose a modelled call's outcome from the scenario's domain; a device loss is sticky.
 * @param kind FK_CALL_*.
 * @param allowed The FK_OUT_* bits the call can return.
 * @param site Choice name.
 * @return The VkResult chosen. */
static VkResult fk_outcome(int kind, unsigned allowed, const char *site)
{
    if (s_lost) return VK_ERROR_DEVICE_LOST;
    const int ord = s_call_ordinal[kind]++;
    for (int i = 0; i < s_nforce; i++)
        if (s_force[i].kind == kind && s_force[i].ordinal == ord) {
            const VkResult fr = (VkResult)s_force[i].result;
            if (fr == VK_ERROR_DEVICE_LOST) s_lost = 1;
            return fr;
        }
    const unsigned m = s_dom.outcomes[kind] & allowed;
    VkResult alt[6];
    int n = 0;
    alt[n++] = VK_SUCCESS;
    if (m & FK_OUT_SUBOPTIMAL)  alt[n++] = VK_SUBOPTIMAL_KHR;
    if (m & FK_OUT_TIMEOUT)     alt[n++] = VK_TIMEOUT;
    if (m & FK_OUT_NOT_READY)   alt[n++] = VK_NOT_READY;
    if (m & FK_OUT_OUT_OF_DATE) alt[n++] = VK_ERROR_OUT_OF_DATE_KHR;
    if (m & FK_OUT_DEVICE_LOST) alt[n++] = VK_ERROR_DEVICE_LOST;
    const VkResult r = alt[sched_choose(n, site)];
    if (r == VK_ERROR_DEVICE_LOST) s_lost = 1;
    return r;
}

/** @brief Sleep the calling thread to a deadline, or not at all outside a run.
 * @param t The deadline.
 * @param ra Call site. */
static void fk_sleep_until(int64_t t, const void *ra) { if (sched_active()) sched_sleep_until(t, ra); }

/** @brief Convert a Vulkan timeout to a deadline.
 * @param to Nanoseconds, UINT64_MAX for none.
 * @return The deadline, or -1 for none. */
static int64_t fk_deadline(uint64_t to)
{
    if (to >= (uint64_t)INT64_MAX / 2) return -1;
    return sched_now() + (int64_t)to;
}

/** @brief GPU completion of a submission: signal its fence and semaphores.
 * @param p The submission. */
static void sub_complete(void *p)
{
    fk_sub *sb = p;
    if (sb->complete) return;
    sb->complete = 1;
    if (sb->fence && sb->fence->pending == sb) { sb->fence->pending = NULL; sb->fence->signaled = 1; }
    for (int i = 0; i < sb->nsig; i++)
        if (sb->sig[i]->pending == sb) { sb->sig[i]->pending = NULL; sb->sig[i]->signaled = 1; }
}

/** @brief Apply a command buffer's blits in record order: each destination takes its source's content.
 * @param cb The command buffer. */
static void cb_execute(const fk_cb *cb)
{
    for (int i = 0; i < cb->nblit; i++) {
        const fk_blit *b = &cb->blit[i];
        b->dst->content = b->from_src ? b->src->content : b->val;
        b->dst->assigned = 1;
        if (b->dst->sc) b->dst->fresh = 1;
    }
}

/** @brief A flip: the image goes on screen and the one it replaces becomes available.
 * @param p The flip record. */
static void flip_event(void *p)
{
    const fk_flip *f = p;
    fk_sc *sc = f->sc;
    if (sc->destroyed) return;
    if (sc->shown >= 0 && sc->img[sc->shown].state == IMG_SHOWN) sc->img[sc->shown].state = IMG_AVAIL;
    sc->img[f->idx].state = IMG_SHOWN;
    sc->shown = (int)f->idx;
    if (f->id > sc->flipped_id) sc->flipped_id = f->id;
}

/* ---- fences and semaphores ---- */
static VkResult f_CreateFence(VkDevice d, const VkFenceCreateInfo *ci, const VkAllocationCallbacks *a, VkFence *o)
{
    (void)d; (void)a; R(vkCreateFence);
    fk_fence *f = calloc(1, sizeof *f);
    f->signaled = (ci->flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0;
    *o = (VkFence)(uintptr_t)f;
    return VK_SUCCESS;
}
static void f_DestroyFence(VkDevice d, VkFence h, const VkAllocationCallbacks *a)
{
    (void)d; (void)a; R(vkDestroyFence);
    fk_fence *f = (fk_fence *)(uintptr_t)h;
    if (!f) return;
    if (f->pending && sched_active()) sched_fail("fakevk: vkDestroyFence on a fence with a pending signal");
    f->destroyed = 1;
}
static VkResult f_CreateSemaphore(VkDevice d, const VkSemaphoreCreateInfo *ci, const VkAllocationCallbacks *a, VkSemaphore *o)
{
    (void)d; (void)ci; (void)a; R(vkCreateSemaphore);
    *o = (VkSemaphore)(uintptr_t)calloc(1, sizeof(fk_sem));
    return VK_SUCCESS;
}
static void f_DestroySemaphore(VkDevice d, VkSemaphore h, const VkAllocationCallbacks *a)
{
    (void)d; (void)a; R(vkDestroySemaphore);
    fk_sem *s = (fk_sem *)(uintptr_t)h;
    if (!s) return;
    if (s->pending && sched_active()) sched_fail("fakevk: vkDestroySemaphore on a semaphore with a pending signal");
    s->destroyed = 1;
}

/** Arguments of a fence-wait predicate. */
typedef struct fk_fwait { uint32_t n; const VkFence *f; VkBool32 all; } fk_fwait;
/** @brief Report whether a fence wait is satisfied.
 * @param p The wait.
 * @return 1 when satisfied. */
static int pred_fences(void *p)
{
    const fk_fwait *w = p;
    uint32_t sig = 0;
    for (uint32_t i = 0; i < w->n; i++) sig += ((fk_fence *)(uintptr_t)w->f[i])->signaled != 0;
    return w->all ? sig == w->n : sig > 0;
}
static VkResult f_WaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t to)
{
    (void)d; R(vkWaitForFences);
    fk_call c;
    fk_begin(&c, "vkWaitForFences", NULL, NULL, n ? (const void *)(uintptr_t)f[0] : NULL, RA);
    VkResult r = fk_outcome(FK_CALL_FENCE_WAIT, FK_OUT_DEVICE_LOST, "fence-wait");
    fk_fwait w = { n, f, all };
    if (r == VK_SUCCESS && !pred_fences(&w)) {
        for (uint32_t i = 0; i < n; i++) {
            const fk_fence *fe = (fk_fence *)(uintptr_t)f[i];
            if (!fe->signaled && !fe->pending && fk_deadline(to) < 0 && sched_active())
                sched_fail("fakevk: unbounded vkWaitForFences on a fence with no pending signal");
        }
        r = (sched_active() ? sched_wait_pred(pred_fences, &w, fk_deadline(to), (const void *)(uintptr_t)f[0], RA)
                            : pred_fences(&w)) ? VK_SUCCESS : VK_TIMEOUT;
    }
    fk_end(&c, NULL);
    return r;
}
static VkResult f_GetFenceStatus(VkDevice d, VkFence h)
{
    (void)d; R(vkGetFenceStatus);
    fk_call c;
    fk_begin(&c, "vkGetFenceStatus", NULL, NULL, (const void *)(uintptr_t)h, RA);
    VkResult r = s_lost ? VK_ERROR_DEVICE_LOST : ((fk_fence *)(uintptr_t)h)->signaled ? VK_SUCCESS : VK_NOT_READY;
    fk_end(&c, NULL);
    return r;
}
static VkResult f_ResetFences(VkDevice d, uint32_t n, const VkFence *f)
{
    (void)d; R(vkResetFences);
    for (uint32_t i = 0; i < n; i++) {
        fk_fence *fe = (fk_fence *)(uintptr_t)f[i];
        if (fe->pending && sched_active()) sched_fail("fakevk: vkResetFences on a fence with a pending signal");
        fe->signaled = 0;
    }
    return VK_SUCCESS;
}

/* ---- images and command buffers ---- */
static VkResult f_CreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *o)
{
    (void)d; (void)ci; (void)a; R(vkCreateImage);
    *o = (VkImage)(uintptr_t)calloc(1, sizeof(fk_image));
    return VK_SUCCESS;
}
static void f_DestroyImage(VkDevice d, VkImage h, const VkAllocationCallbacks *a) { (void)d; (void)h; (void)a; R(vkDestroyImage); }
static void f_GetImageMemoryRequirements(VkDevice d, VkImage i, VkMemoryRequirements *r)
{ (void)d; (void)i; R(vkGetImageMemoryRequirements); r->size = 1 << 20; r->alignment = 256; r->memoryTypeBits = 0x7; }
static VkResult f_AllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *o)
{
    (void)d; R(vkAllocateCommandBuffers);
    for (uint32_t i = 0; i < ai->commandBufferCount; i++) o[i] = (VkCommandBuffer)calloc(1, sizeof(fk_cb));
    return VK_SUCCESS;
}
static void f_FreeCommandBuffers(VkDevice d, VkCommandPool p, uint32_t n, const VkCommandBuffer *c) { (void)d; (void)p; (void)n; (void)c; R(vkFreeCommandBuffers); }
static VkResult f_BeginCommandBuffer(VkCommandBuffer c, const VkCommandBufferBeginInfo *b) { (void)b; R(vkBeginCommandBuffer); ((fk_cb *)c)->nblit = 0; return VK_SUCCESS; }
static VkResult f_EndCommandBuffer(VkCommandBuffer c) { (void)c; R(vkEndCommandBuffer); return VK_SUCCESS; }
static VkResult f_ResetCommandBuffer(VkCommandBuffer c, VkCommandBufferResetFlags f) { (void)f; R(vkResetCommandBuffer); ((fk_cb *)c)->nblit = 0; return VK_SUCCESS; }
static void f_CmdBlitImage(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f)
{
    (void)sl; (void)dl; (void)n; (void)r; (void)f; R(vkCmdBlitImage);
    fk_cb *cb = (fk_cb *)c;
    fk_image *src = (fk_image *)(uintptr_t)s, *dst = (fk_image *)(uintptr_t)d;
    if (cb->nblit >= FK_CB_BLITS) return;
    /* A source no blit ever wrote is a render target: it holds the compose in progress. */
    cb->blit[cb->nblit++] = (fk_blit){ src, dst, s_render, src->assigned };
}

/* ---- queue ---- */

/** @brief Apply one batch of a submission: consume its wait semaphores, arm its signal semaphores, execute its command buffers.
 * @param sb The submission.
 * @param si The batch.
 * @param act Nonzero inside a scheduled run, where a broken rule fails the schedule. */
static void submit_batch(fk_sub *sb, const VkSubmitInfo *si, int act)
{
    for (uint32_t k = 0; k < si->waitSemaphoreCount; k++) {
        fk_sem *sm = (fk_sem *)(uintptr_t)si->pWaitSemaphores[k];
        if (!sm->signaled && !sm->pending && act) sched_fail("fakevk: vkQueueSubmit waits a semaphore with no signal pending (GPU deadlock)");
        sm->signaled = 0; sm->pending = NULL;
    }
    for (uint32_t k = 0; k < si->signalSemaphoreCount && sb->nsig < 4; k++) {
        fk_sem *sm = (fk_sem *)(uintptr_t)si->pSignalSemaphores[k];
        if ((sm->signaled || sm->pending) && act) sched_fail("fakevk: vkQueueSubmit signals a semaphore already signalled or pending");
        sm->pending = sb;
        sb->sig[sb->nsig++] = sm;
    }
    for (uint32_t k = 0; k < si->commandBufferCount; k++) cb_execute((const fk_cb *)si->pCommandBuffers[k]);
}
static VkResult f_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence fence)
{
    (void)q; R(vkQueueSubmit);
    fk_call c;
    fk_begin(&c, "vkQueueSubmit", &s_queue_inside, NULL, &g_queue, RA);
    VkResult r = fk_outcome(FK_CALL_SUBMIT, FK_OUT_DEVICE_LOST, "submit");
    if (r != VK_SUCCESS) { fk_end(&c, NULL); return r; }
    fk_fence *fe = (fk_fence *)(uintptr_t)fence;
    const int act = sched_active();
    if (fe && (fe->signaled || fe->pending) && act) sched_fail("fakevk: vkQueueSubmit with a fence that is signalled or pending");
    const int64_t lat = s_dom.gpu_ns[sched_choose(s_dom.n_gpu, "gpu-latency")];
    fk_sub *sb = calloc(1, sizeof *sb);
    sb->seq = ++s_sub_seq;
    const int64_t now = sched_now();
    sb->done_at = (s_last_done > now ? s_last_done : now) + lat;
    s_last_done = sb->done_at;
    for (uint32_t i = 0; i < n; i++) submit_batch(sb, &si[i], act);
    if (fe) { fe->pending = sb; sb->fence = fe; }
    s_last_sub = sb;
    if (!act || sb->done_at <= now) sub_complete(sb);
    else sched_at(sb->done_at, sub_complete, sb);
    fk_end(&c, NULL);
    return VK_SUCCESS;
}

/** @brief Report whether every submission has completed.
 * @param p Unused.
 * @return 1 when the GPU is idle. */
static int pred_idle(void *p) { (void)p; return s_last_sub == NULL || s_last_sub->complete; }
static VkResult f_DeviceWaitIdle(VkDevice d)
{
    (void)d; R(vkDeviceWaitIdle);
    fk_call c;
    fk_begin(&c, "vkDeviceWaitIdle", &s_queue_inside, NULL, &g_dev, RA);
    if (!s_lost && !pred_idle(NULL) && sched_active()) sched_wait_pred(pred_idle, NULL, -1, &g_dev, RA);
    fk_end(&c, NULL);
    return s_lost ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}
static VkResult f_QueueWaitIdle(VkQueue q)
{
    (void)q; R(vkQueueWaitIdle);
    fk_call c;
    fk_begin(&c, "vkQueueWaitIdle", &s_queue_inside, NULL, &g_queue, RA);
    if (!s_lost && !pred_idle(NULL) && sched_active()) sched_wait_pred(pred_idle, NULL, -1, &g_queue, RA);
    fk_end(&c, NULL);
    return s_lost ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

/* ---- swapchain ---- */
static VkResult f_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice d, VkSurfaceKHR s, VkSurfaceCapabilitiesKHR *c)
{
    (void)d; (void)s; R(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); memset(c, 0, sizeof *c);
    c->minImageCount = 2; c->maxImageCount = FK_SC_MAX;
    c->currentExtent = (VkExtent2D){ (uint32_t)s_win_w, (uint32_t)s_win_h };
    c->minImageExtent = (VkExtent2D){ 0, 0 }; c->maxImageExtent = (VkExtent2D){ 16384, 16384 };
    c->maxImageArrayLayers = 1; c->supportedTransforms = c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; c->supportedUsageFlags = 0xff;
    return VK_SUCCESS;
}
static VkResult f_CreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSwapchainKHR *o)
{
    (void)d; (void)a; R(vkCreateSwapchainKHR);
    fk_call c;
    fk_begin(&c, "vkCreateSwapchainKHR", NULL, NULL, &g_dev, RA);
    fk_sc *sc = calloc(1, sizeof *sc);
    sc->n = ci->minImageCount < FK_SC_MAX ? ci->minImageCount : FK_SC_MAX;
    sc->shown = -1; sc->inside = -1;
    for (uint32_t i = 0; i < sc->n; i++) { sc->img[i].sc = sc; sc->img[i].state = IMG_AVAIL; sc->img[i].acquirer = -1; }
    fk_sc *old = (fk_sc *)(uintptr_t)ci->oldSwapchain;
    if (old) old->retired = 1;
    s_cur_sc = sc;
    *o = (VkSwapchainKHR)(uintptr_t)sc;
    fk_end(&c, NULL);
    return VK_SUCCESS;
}
static void f_DestroySwapchainKHR(VkDevice d, VkSwapchainKHR h, const VkAllocationCallbacks *a)
{
    (void)d; (void)a; R(vkDestroySwapchainKHR);
    fk_sc *sc = (fk_sc *)(uintptr_t)h;
    if (!sc) return;
    fk_call c;
    fk_begin(&c, "vkDestroySwapchainKHR", &sc->inside, NULL, sc, RA);
    sc->destroyed = 1;
    if (s_cur_sc == sc) s_cur_sc = NULL;
    fk_end(&c, NULL);
}
static VkResult f_GetSwapchainImagesKHR(VkDevice d, VkSwapchainKHR s, uint32_t *n, VkImage *img)
{
    (void)d; R(vkGetSwapchainImagesKHR);
    fk_sc *sc = (fk_sc *)(uintptr_t)s;
    if (img) for (uint32_t i = 0; i < sc->n && i < *n; i++) img[i] = (VkImage)(uintptr_t)&sc->img[i];
    if (!img || *n >= sc->n) *n = sc->n;
    return VK_SUCCESS;
}

/** @brief Report whether a swapchain has an image to hand out.
 * @param p The swapchain.
 * @return 1 when one is available. */
static int pred_avail(void *p)
{
    const fk_sc *sc = p;
    if (sc->destroyed || sc->retired) return 1;
    for (uint32_t i = 0; i < sc->n; i++)
        if (sc->img[i].state == IMG_AVAIL) return 1;
    return 0;
}
/** @brief Hand out an available image of a swapchain, waiting up to the acquire's timeout: the acquire's success path.
 *
 * Checks the RHI's one-outstanding-acquire invariant and that the semaphore
 * is free, then takes the first available image for the calling thread and
 * signals the semaphore and fence.
 * @param sc The swapchain.
 * @param to Timeout in nanoseconds.
 * @param sm Semaphore to signal, or NULL.
 * @param fe Fence to signal, or NULL.
 * @param ix Receives the image index.
 * @return VK_SUCCESS, VK_TIMEOUT, VK_NOT_READY or VK_ERROR_OUT_OF_DATE_KHR. */
static VkResult sc_take_image(fk_sc *sc, uint64_t to, fk_sem *sm, fk_fence *fe, uint32_t *ix)
{
    if (sc->retired || sc->destroyed || s_win_w <= 0 || s_win_h <= 0) return VK_ERROR_OUT_OF_DATE_KHR;
    const int act = sched_active();
    if (act && fakevk_acquired_outstanding() > 0)
        sched_fail("P-ACQUIRE: vkAcquireNextImageKHR with an image already acquired and not presented");
    if (act && sm && (sm->signaled || sm->pending))
        sched_fail("fakevk: vkAcquireNextImageKHR signals a semaphore already signalled or pending");
    int ok = pred_avail(sc);
    if (!ok && to > 0 && act) ok = sched_wait_pred(pred_avail, sc, fk_deadline(to), sc, RA);
    if (ok && (sc->retired || sc->destroyed)) return VK_ERROR_OUT_OF_DATE_KHR;
    if (!ok) return to == 0 ? VK_NOT_READY : VK_TIMEOUT;
    uint32_t k = 0;
    while (sc->img[k].state != IMG_AVAIL) k++;
    sc->img[k].state = IMG_ACQUIRED;
    sc->img[k].acquirer = sched_self();
    sc->img[k].fresh = 0;
    if (sm) sm->signaled = 1;
    if (fe) fe->signaled = 1;
    *ix = k;
    return VK_SUCCESS;
}

static VkResult f_AcquireNextImageKHR(VkDevice d, VkSwapchainKHR s, uint64_t to, VkSemaphore sem, VkFence f, uint32_t *ix)
{
    (void)d; R(vkAcquireNextImageKHR);
    fk_sc *sc = (fk_sc *)(uintptr_t)s;
    fk_call c;
    fk_begin(&c, "vkAcquireNextImageKHR", &sc->inside, NULL, sc, RA);
    VkResult r = fk_outcome(FK_CALL_ACQUIRE, FK_OUT_SUBOPTIMAL | FK_OUT_TIMEOUT | FK_OUT_NOT_READY |
                            FK_OUT_OUT_OF_DATE | FK_OUT_DEVICE_LOST, "acquire");
    if (r == VK_TIMEOUT && fk_deadline(to) >= 0) fk_sleep_until(fk_deadline(to), RA);
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
        const VkResult t = sc_take_image(sc, to, (fk_sem *)(uintptr_t)sem, (fk_fence *)(uintptr_t)f, ix);
        if (t != VK_SUCCESS) r = t;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR && sched_active()) s_acq_refusals++;
    fk_end(&c, NULL);
    return r;
}
/** @brief Read a present's VkPresentIdKHR and record it on the swapchain, checking P-PRESENT's id clause.
 *
 * A present that carries the id chain carries an id vkWaitForPresentKHR can
 * confirm: nonzero (0 is VK_KHR_present_id's "no id") and above every id
 * queued on the swapchain before it.
 * @param sc The swapchain presented to.
 * @param pi The present.
 * @param act Nonzero inside a schedule, where a violation fails it.
 * @return The present id, 0 when the present carries none. */
static uint64_t present_id_take(fk_sc *sc, const VkPresentInfoKHR *pi, int act)
{
    for (const VkBaseInStructure *p = pi->pNext; p; p = p->pNext) {
        if (p->sType != VK_STRUCTURE_TYPE_PRESENT_ID_KHR) continue;
        const uint64_t id = ((const VkPresentIdKHR *)p)->pPresentIds[0];
        if (act && id <= sc->presented_id)
            sched_fail("P-PRESENT: vkQueuePresentKHR carries present id %llu after id %llu on its swapchain (ids start "
                       "at 1 and increase)", (unsigned long long)id, (unsigned long long)sc->presented_id);
        if (id > sc->presented_id) sc->presented_id = id;
        return id;
    }
    return 0;
}
static VkResult f_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi)
{
    (void)q; R(vkQueuePresentKHR);
    fk_sc *sc = (fk_sc *)(uintptr_t)pi->pSwapchains[0];
    const uint32_t idx = pi->pImageIndices[0];
    fk_call c;
    fk_begin(&c, "vkQueuePresentKHR", &s_queue_inside, &sc->inside, sc, RA);
    const int act = sched_active();
    fk_image *im = &sc->img[idx];
    if (act && im->state != IMG_ACQUIRED)
        sched_fail("P-PRESENT: vkQueuePresentKHR of image %u, which is not acquired (presented twice or never acquired)", idx);
    int64_t ready = sched_now();
    for (uint32_t k = 0; k < pi->waitSemaphoreCount; k++) {
        fk_sem *sm = (fk_sem *)(uintptr_t)pi->pWaitSemaphores[k];
        if (!sm->signaled && !sm->pending && act)
            sched_fail("fakevk: vkQueuePresentKHR waits a semaphore no submission signals");
        if (sm->pending && sm->pending->done_at > ready) ready = sm->pending->done_at;
        sm->signaled = 0; sm->pending = NULL;
    }
    const uint64_t id = present_id_take(sc, pi, act);
    VkResult r = fk_outcome(FK_CALL_PRESENT, FK_OUT_SUBOPTIMAL | FK_OUT_OUT_OF_DATE | FK_OUT_DEVICE_LOST, "present");
    if (r == VK_SUCCESS && (sc->retired || sc->destroyed)) r = VK_ERROR_OUT_OF_DATE_KHR;
    if (s_hooks.shown && act) {
        const fakevk_shown sh = { im->content, im->fresh, sched_self(), im->acquirer, (int)r };
        s_hooks.shown(&sh);
    }
    im->fresh = 0;
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
        im->state = IMG_QUEUED;
        const int64_t p = s_dom.vblank_ns;
        int64_t at = (ready / p + 1) * p;
        if (at <= sc->last_flip_at) at = sc->last_flip_at + p;
        sc->last_flip_at = at;
        fk_flip *fl = malloc(sizeof *fl);
        *fl = (fk_flip){ sc, idx, id };
        if (act) sched_at(at, flip_event, fl);
        else flip_event(fl);
    } else {
        im->state = IMG_AVAIL;
    }
    fk_end(&c, &sc->inside);
    return r;
}

/** Arguments of a present-wait predicate. */
typedef struct fk_pwait { const fk_sc *sc; uint64_t id; } fk_pwait;
/** @brief Report whether a present id has flipped or its swapchain is gone.
 * @param p The wait.
 * @return 1 when the wait is over. */
static int pred_flipped(void *p)
{
    const fk_pwait *w = p;
    return w->sc->flipped_id >= w->id || w->sc->destroyed || w->sc->retired;
}
static VkResult f_WaitForPresentKHR(VkDevice d, VkSwapchainKHR s, uint64_t id, uint64_t to)
{
    (void)d; R(vkWaitForPresentKHR);
    fk_sc *sc = (fk_sc *)(uintptr_t)s;
    fk_call c;
    fk_begin(&c, "vkWaitForPresentKHR", &sc->inside, NULL, sc, RA);
    VkResult r = fk_outcome(FK_CALL_PRESENT_WAIT, FK_OUT_DEVICE_LOST, "present-wait");
    if (r == VK_SUCCESS) {
        fk_pwait w = { sc, id };
        const int ok = pred_flipped(&w) || (sched_active() && sched_wait_pred(pred_flipped, &w, fk_deadline(to), sc, RA));
        r = !ok ? VK_TIMEOUT : (sc->destroyed || sc->retired) && sc->flipped_id < id ? VK_ERROR_OUT_OF_DATE_KHR : VK_SUCCESS;
    }
    fk_end(&c, NULL);
    return r;
}

/* ---- the window's event watch ---- */
bool SDL_AddEventWatch(SDL_EventFilter f, void *u);
/** @brief Record the RHI's event watch: the harness raises display changes through it.
 * @param f The watch.
 * @param u Its user data (the device).
 * @return true. */
bool SDL_AddEventWatch(SDL_EventFilter f, void *u) { s_watch_fn = f; s_watch_ud = u; return true; }
void SDL_RemoveEventWatch(SDL_EventFilter f, void *u);
/** @brief Forget the RHI's event watch.
 * @param f The watch.
 * @param u Its user data. */
void SDL_RemoveEventWatch(SDL_EventFilter f, void *u) { (void)f; (void)u; s_watch_fn = NULL; }

void fakevk_display_change(void)
{
    if (!s_watch_fn) return;
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_EVENT_WINDOW_DISPLAY_CHANGED;
    ev.window.windowID = 1;
    s_watch_fn(s_watch_ud, &ev);
}

const void *fakevk_display_changed_addr(void)
{
    if (!s_changed_addr && s_watch_fn && !sched_active()) {
        (void)sched_watch_probe(1);
        fakevk_display_change();
        s_changed_addr = sched_watch_probe(0);
        /* Consume the probe's flag so every schedule starts with no change pending. */
        if (s_changed_addr) *(volatile _Bool *)s_changed_addr = 0;
    }
    return s_changed_addr;
}

int fakevk_report(int verbose)
{
    int k = 0;
#define X(n) if (reach_##n) { if (verbose) printf("  %-46s %lu\n", #n, reach_##n); k++; }
#include "vk_list.h"
#undef X
    return k;
}

/** @brief The harness's volkInitialize: every referenced pointer a trap, then the modelled ones installed.
 * @return VK_SUCCESS. */
VkResult volkInitialize(void)
{
#define X(n) n = (PFN_##n)(void (*)(void))trap_##n;
#include "vk_list.h"
#undef X
#define M(n) vk##n = f_##n;
    M(CreateInstance) M(DestroyInstance) M(EnumerateInstanceLayerProperties) M(GetInstanceProcAddr)
    M(EnumeratePhysicalDevices) M(GetPhysicalDeviceProperties) M(GetPhysicalDeviceProperties2)
    M(GetPhysicalDeviceFeatures) M(GetPhysicalDeviceFeatures2) M(GetPhysicalDeviceFormatProperties)
    M(GetPhysicalDeviceMemoryProperties) M(GetPhysicalDeviceQueueFamilyProperties)
    M(EnumerateDeviceExtensionProperties) M(EnumerateDeviceLayerProperties)
    M(GetPhysicalDeviceSurfaceSupportKHR) M(GetPhysicalDeviceSurfaceCapabilitiesKHR)
    M(GetPhysicalDeviceSurfaceFormatsKHR) M(GetPhysicalDeviceSurfacePresentModesKHR) M(DestroySurfaceKHR)
    M(CreateDevice) M(DestroyDevice) M(GetDeviceQueue) M(DeviceWaitIdle) M(QueueWaitIdle)
    M(CreateBuffer) M(DestroyBuffer) M(CreateImageView) M(DestroyImageView) M(CreateCommandPool) M(DestroyCommandPool)
    M(CreateDescriptorPool) M(DestroyDescriptorPool) M(CreateDescriptorSetLayout) M(DestroyDescriptorSetLayout)
    M(CreateFramebuffer) M(DestroyFramebuffer) M(CreatePipelineCache) M(DestroyPipelineCache)
    M(CreatePipelineLayout) M(DestroyPipelineLayout) M(CreateQueryPool) M(DestroyQueryPool)
    M(CreateRenderPass) M(DestroyRenderPass) M(CreateSampler) M(DestroySampler) M(CreateSemaphore) M(DestroySemaphore)
    M(CreateShaderModule) M(DestroyShaderModule) M(DestroyPipeline) M(DestroyImage) M(DestroySwapchainKHR)
    M(CreateFence) M(DestroyFence) M(WaitForFences) M(GetFenceStatus) M(ResetFences) M(QueueSubmit)
    M(AllocateMemory) M(FreeMemory) M(MapMemory) M(UnmapMemory) M(BindBufferMemory) M(BindImageMemory)
    M(CreateImage) M(GetImageMemoryRequirements) M(GetBufferMemoryRequirements)
    M(AllocateCommandBuffers) M(FreeCommandBuffers) M(BeginCommandBuffer) M(EndCommandBuffer) M(ResetCommandBuffer)
    M(AllocateDescriptorSets) M(FreeDescriptorSets) M(UpdateDescriptorSets)
    M(CreateGraphicsPipelines) M(CreateComputePipelines) M(GetPipelineCacheData) M(GetQueryPoolResults)
    M(CreateSwapchainKHR) M(GetSwapchainImagesKHR) M(AcquireNextImageKHR) M(QueuePresentKHR) M(WaitForPresentKHR)
    M(CmdBeginRenderPass) M(CmdEndRenderPass) M(CmdBindDescriptorSets) M(CmdBindPipeline) M(CmdBindVertexBuffers)
    M(CmdBlitImage) M(CmdClearAttachments) M(CmdClearColorImage) M(CmdCopyBuffer) M(CmdCopyBufferToImage)
    M(CmdCopyImageToBuffer) M(CmdDispatch) M(CmdDraw) M(CmdFillBuffer) M(CmdPipelineBarrier) M(CmdPushConstants)
    M(CmdResetQueryPool) M(CmdSetDepthBias) M(CmdSetScissor) M(CmdSetViewport) M(CmdWriteTimestamp)
#undef M
    return VK_SUCCESS;
}
void volkLoadInstance(VkInstance i) { (void)i; }
void volkLoadDevice(VkDevice d) { (void)d; }
