/*
 * openpete RHI — Vulkan backend (milestone 1a).
 *
 * Brings up an instance/device/swapchain, an offscreen color target the
 * size of the swapchain, and per-frame: clear the offscreen target via a
 * render pass, blit it onto the acquired swapchain image, present. This is
 * the standard Vulkan bringup; later milestones add the VRAM image,
 * pipelines, vertex buffers, descriptor sets, and readback behind the same
 * rhi.h seam.
 *
 * Sync model: imageAvailable + inFlight fence per frame-in-flight;
 * renderFinished semaphore per swapchain image; an imagesInFlight fence
 * map to avoid reusing a swapchain image still in flight.
 */

#include "rhi.h"
#include "rhi_embedded_shaders.h" /* g_rhi_embedded_shaders[] — generated */

#include "volk.h"          /* dynamic Vulkan loader; pulls in vulkan.h with
                            * VK_NO_PROTOTYPES. Must precede SDL_vulkan.h. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#define RHI_FRAMES_IN_FLIGHT 2
#define RHI_MAX_SC_IMAGES    8

#define RHI_LOG(...)  fprintf(stderr, "[rhi/vk] " __VA_ARGS__)

/* Handler invoked on an unrecoverable VK_ERROR_DEVICE_LOST (registered by the
 * core layer, which owns the shutdown flag) instead of spinning failed frames
 * forever. A callback keeps openpete_rhi free of core-runtime symbols — the
 * link goes core -> rhi, not back. */
static void (*s_device_lost_cb)(void) = NULL;
void rhi_set_device_lost_handler(void (*fn)(void)) { s_device_lost_cb = fn; }
static void rhi_signal_device_lost(const char *where) {
    RHI_LOG("FATAL: device lost on %s — requesting shutdown\n", where);
    if (s_device_lost_cb) s_device_lost_cb();
}

#define VK_CHECK(expr)                                                       \
    do {                                                                     \
        VkResult _vr = (expr);                                               \
        if (_vr != VK_SUCCESS) {                                             \
            RHI_LOG("%s failed: VkResult=%d (%s:%d)\n", #expr, (int)_vr,     \
                    __FILE__, __LINE__);                                     \
        }                                                                    \
    } while (0)

struct rhi_device {
    struct SDL_Window *window;

    VkInstance               instance;
    VkDebugUtilsMessengerEXT debug;
    VkSurfaceKHR             surface;
    VkPhysicalDevice         phys;
    VkDevice                 device;
    uint32_t                 gfx_family;
    VkQueue                  gfx_queue;
    char                     device_name[256];
    bool                     headless;   /* surfaceless: no swapchain / present */

    /* Swapchain */
    VkSwapchainKHR swapchain;
    VkFormat       sc_format;
    VkExtent2D     sc_extent;
    uint32_t       sc_count;
    VkImage        sc_images[RHI_MAX_SC_IMAGES];

    /* Offscreen color target (== swapchain extent). SSAA supersampling lives in
     * the renderer as a separate render target that resolves into this. */
    VkRenderPass   off_rp;
    /* Render-to-texture pass: same attachment formats as off_rp (so off_rp-built
     * pipelines are compatible) but the color attachment ends in
     * SHADER_READ_ONLY_OPTIMAL so the result can be sampled the same frame. */
    VkRenderPass   rt_rp;
    VkImage        off_image;
    VkDeviceMemory off_mem;
    VkImageView    off_view;
    VkFramebuffer  off_fb;

    /* Depth attachment for the offscreen target (3a.3 native-renderer depth):
     * D32_SFLOAT, cleared to 1.0 each frame, store-discarded. Pipelines opt in
     * via rhi_pipeline_desc.depth_test; the 2D panels leave it off. */
    VkImage        off_depth_image;
    VkDeviceMemory off_depth_mem;
    VkImageView    off_depth_view;

    /* Commands + sync */
    VkCommandPool   pool;
    VkCommandBuffer cmd[RHI_FRAMES_IN_FLIGHT];
    VkSemaphore     img_avail[RHI_FRAMES_IN_FLIGHT];
    VkFence         in_flight[RHI_FRAMES_IN_FLIGHT];
    VkSemaphore     render_done[RHI_MAX_SC_IMAGES];
    VkFence         images_in_flight[RHI_MAX_SC_IMAGES]; /* not owned */

    uint32_t frame;        /* 0..RHI_FRAMES_IN_FLIGHT-1 */
    uint32_t image_index;  /* acquired swapchain image this frame */
    bool     needs_recreate;
    bool     frame_active;  /* between begin_frame and end_frame */
    VkExtent2D cur_extent;  /* render area of the CURRENT pass (offscreen sc_extent
                            * or the bound render target) — for rhi_clear_depth */

    float clear[4];

    /* VRAM-sampler infrastructure (1b.2): one shared sampler + set layout +
     * pool; each rhi_texture owns a descriptor set allocated from the pool. */
    VkSampler             vram_sampler;
    VkDescriptorSetLayout vram_set_layout;
    VkDescriptorPool      desc_pool;
    VkPipelineLayout      cur_layout;  /* layout of the bound pipeline */

    /* Reusable host-visible staging buffer for rhi_read_offscreen (the blocking
     * screenshot path). Persistent so it doesn't vkAllocateMemory/free each call;
     * grown on demand. */
    rhi_buffer           *readback_buf;

    /* Async readback ring for --record (rhi_cycle_readback): one slot per frame
     * in flight. The copy into slot i rides cmd[i] under fence in_flight[i], so
     * begin_frame's existing wait on in_flight[frame] guarantees slot[frame]'s
     * previous copy has completed by the time that slot is cycled again — the
     * pixels are retrieved with NO added GPU stall (unlike rhi_read_offscreen's
     * vkDeviceWaitIdle), which is what lets recording run at real speed. */
    struct {
        rhi_buffer *staging;   /* host-visible TRANSFER_DST; grown on demand */
        bool        pending;   /* a copy was queued into cmd[i], not yet retrieved */
        int         w, h;      /* extent captured into staging */
        uint64_t    tag;       /* opaque caller token (the PTS) for this capture */
    } readback_ring[RHI_FRAMES_IN_FLIGHT];
};

struct rhi_buffer {
    VkBuffer       buffer;
    VkDeviceMemory memory;
    void          *mapped;
    VkDeviceSize   size;
};

struct rhi_pipeline {
    VkPipeline       pipeline;
    VkPipelineLayout layout;
};

struct rhi_texture {
    VkImage         image;
    VkDeviceMemory  mem;
    VkImageView     view;
    VkDescriptorSet set;       /* points at (view, dev->vram_sampler) */
    rhi_buffer     *staging;   /* host-visible upload buffer (NULL for render targets) */
    uint32_t        width, height;
    uint32_t        bpp;       /* bytes per pixel (2 = R16_UINT VRAM, 4 = RGBA8) */
    VkImageLayout   layout;    /* current layout, for re-upload transitions */

    /* Render-target extras (is_rt only): a depth attachment + a framebuffer so
     * geometry can be rendered into `image` via dev->rt_rp. */
    bool            is_rt;
    VkImage         rt_depth_image;
    VkDeviceMemory  rt_depth_mem;
    VkImageView     rt_depth_view;
    VkFramebuffer   rt_fb;
};

/* ------------------------------------------------------------------ */

static VKAPI_ATTR VkBool32 VKAPI_CALL
debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
         VkDebugUtilsMessageTypeFlagsEXT type,
         const VkDebugUtilsMessengerCallbackDataEXT *data, void *ud)
{
    (void)type; (void)ud;
    if (sev & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) {
        RHI_LOG("validation: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

static uint32_t find_memtype(VkPhysicalDevice phys, uint32_t type_bits,
                             VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

static bool layer_available(const char *name)
{
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, NULL);
    if (n == 0) return false;
    VkLayerProperties *props = calloc(n, sizeof(*props));
    vkEnumerateInstanceLayerProperties(&n, props);
    bool found = false;
    for (uint32_t i = 0; i < n && !found; i++) {
        if (strcmp(props[i].layerName, name) == 0) found = true;
    }
    free(props);
    return found;
}

/* ------------------------------------------------------------------ */

static bool create_instance(rhi_device *dev, const char *app_name)
{
    /* Windowed: SDL supplies the platform surface extensions. Headless: none —
     * a surfaceless instance needs no surface extension at all. */
    Uint32 sdl_n = 0;
    char const *const *sdl_ext = NULL;
    if (!dev->headless) {
        sdl_ext = SDL_Vulkan_GetInstanceExtensions(&sdl_n);
        if (!sdl_ext) {
            RHI_LOG("SDL_Vulkan_GetInstanceExtensions failed: %s\n", SDL_GetError());
            return false;
        }
    }

    bool want_validation = layer_available("VK_LAYER_KHRONOS_validation");

    /* Copy SDL's required extensions (none when headless), optionally append
     * debug utils. */
    uint32_t ext_n = sdl_n + (want_validation ? 1u : 0u);
    const char **exts = ext_n ? calloc(ext_n, sizeof(*exts)) : NULL;
    for (Uint32 i = 0; i < sdl_n; i++) exts[i] = sdl_ext[i];
    if (want_validation) exts[sdl_n] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;

    const char *layers[] = { "VK_LAYER_KHRONOS_validation" };

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = app_name ? app_name : "openpete",
        .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
        .pEngineName = "openpete-rhi",
        .engineVersion = VK_MAKE_VERSION(0, 1, 0),
        .apiVersion = VK_API_VERSION_1_2,
    };
    VkInstanceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = ext_n,
        .ppEnabledExtensionNames = exts,
        .enabledLayerCount = want_validation ? 1u : 0u,
        .ppEnabledLayerNames = want_validation ? layers : NULL,
    };
    VkResult r = vkCreateInstance(&ci, NULL, &dev->instance);
    free(exts);
    if (r != VK_SUCCESS) {
        RHI_LOG("vkCreateInstance failed: %d\n", (int)r);
        return false;
    }

    if (want_validation) {
        PFN_vkCreateDebugUtilsMessengerEXT mk =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                dev->instance, "vkCreateDebugUtilsMessengerEXT");
        if (mk) {
            VkDebugUtilsMessengerCreateInfoEXT dci = {
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .messageSeverity =
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                .messageType =
                    VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                .pfnUserCallback = debug_cb,
            };
            mk(dev->instance, &dci, NULL, &dev->debug);
        }
        RHI_LOG("validation layer enabled\n");
    }
    return true;
}

static bool pick_physical_and_device(rhi_device *dev)
{
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(dev->instance, &n, NULL);
    if (n == 0) { RHI_LOG("no Vulkan physical devices\n"); return false; }
    VkPhysicalDevice *devs = calloc(n, sizeof(*devs));
    vkEnumeratePhysicalDevices(dev->instance, &n, devs);

    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    uint32_t chosen_family = UINT32_MAX;
    int chosen_score = -1;

    for (uint32_t i = 0; i < n; i++) {
        /* Require a queue family with graphics + present to our surface. */
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, NULL);
        VkQueueFamilyProperties *q = calloc(qn, sizeof(*q));
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, q);
        uint32_t fam = UINT32_MAX;
        for (uint32_t j = 0; j < qn; j++) {
            /* Headless has no surface: a graphics queue is all we need. */
            VkBool32 present = VK_FALSE;
            if (!dev->headless)
                vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], j, dev->surface, &present);
            if ((q[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                (dev->headless || present)) {
                fam = j; break;
            }
        }
        free(q);
        if (fam == UINT32_MAX) continue;

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devs[i], &props);
        /* Prefer real hardware over the software rasterizer (llvmpipe
         * reports as CPU): discrete > integrated > virtual > other > cpu. */
        int score;
        switch (props.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   score = 4; break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 3; break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    score = 2; break;
            case VK_PHYSICAL_DEVICE_TYPE_CPU:            score = 0; break;
            default:                                     score = 1; break;
        }
        if (score > chosen_score) {
            chosen = devs[i];
            chosen_family = fam;
            chosen_score = score;
            snprintf(dev->device_name, sizeof(dev->device_name), "%s", props.deviceName);
        }
    }
    free(devs);

    if (chosen == VK_NULL_HANDLE) {
        RHI_LOG("no device with a graphics%s queue\n",
                dev->headless ? "" : "+present");
        return false;
    }
    dev->phys = chosen;
    dev->gfx_family = chosen_family;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = chosen_family,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    const char *dev_ext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        /* No swapchain extension when there's no surface to present to. */
        .enabledExtensionCount = dev->headless ? 0u : 1u,
        .ppEnabledExtensionNames = dev->headless ? NULL : dev_ext,
    };
    VkResult r = vkCreateDevice(dev->phys, &dci, NULL, &dev->device);
    if (r != VK_SUCCESS) { RHI_LOG("vkCreateDevice failed: %d\n", (int)r); return false; }
    vkGetDeviceQueue(dev->device, chosen_family, 0, &dev->gfx_queue);
    return true;
}

static VkExtent2D choose_extent(rhi_device *dev, const VkSurfaceCapabilitiesKHR *caps)
{
    if (caps->currentExtent.width != UINT32_MAX) return caps->currentExtent;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(dev->window, &w, &h);
    VkExtent2D e = { (uint32_t)w, (uint32_t)h };
    if (e.width  < caps->minImageExtent.width)  e.width  = caps->minImageExtent.width;
    if (e.width  > caps->maxImageExtent.width)  e.width  = caps->maxImageExtent.width;
    if (e.height < caps->minImageExtent.height) e.height = caps->minImageExtent.height;
    if (e.height > caps->maxImageExtent.height) e.height = caps->maxImageExtent.height;
    return e;
}

/* Swapchain present mode. Default FIFO (vsync — always supported, locks present
 * to the monitor refresh). OPENPETE_PRESENT_MODE=immediate|mailbox selects a
 * vsync-OFF mode for low latency or uncapped-throughput benchmarking (FIFO
 * otherwise hides the real frame rate behind the refresh ceiling). Falls back to
 * FIFO with a log if the requested mode isn't supported by the surface. */
static VkPresentModeKHR choose_present_mode(rhi_device *dev)
{
    const char *env = getenv("OPENPETE_PRESENT_MODE");
    VkPresentModeKHR want = VK_PRESENT_MODE_FIFO_KHR;
    if (env && !strcmp(env, "immediate")) want = VK_PRESENT_MODE_IMMEDIATE_KHR;
    else if (env && !strcmp(env, "mailbox")) want = VK_PRESENT_MODE_MAILBOX_KHR;
    if (want == VK_PRESENT_MODE_FIFO_KHR) return want;  /* always supported */

    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev->phys, dev->surface, &n, NULL);
    VkPresentModeKHR *modes = calloc(n ? n : 1, sizeof(*modes));
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev->phys, dev->surface, &n, modes);
    bool ok = false;
    for (uint32_t i = 0; i < n; i++) if (modes[i] == want) { ok = true; break; }
    free(modes);
    if (!ok) {
        RHI_LOG("present mode '%s' unsupported by surface; using FIFO (vsync)\n", env);
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    RHI_LOG("present mode: %s (vsync OFF)\n", env);
    return want;
}

static bool create_swapchain(rhi_device *dev)
{
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev->phys, dev->surface, &caps);

    VkExtent2D extent = choose_extent(dev, &caps);
    if (extent.width == 0 || extent.height == 0) return false; /* minimized */

    /* Format: prefer B8G8R8A8_UNORM / SRGB nonlinear, else first. */
    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev->phys, dev->surface, &fn, NULL);
    if (fn == 0) { RHI_LOG("create_swapchain: surface reports zero formats\n"); return false; }
    VkSurfaceFormatKHR *fmts = calloc(fn, sizeof(*fmts));
    if (!fmts) { RHI_LOG("create_swapchain: OOM for %u surface formats\n", fn); return false; }
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev->phys, dev->surface, &fn, fmts);
    VkSurfaceFormatKHR pick = fmts[0];
    for (uint32_t i = 0; i < fn; i++) {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            pick = fmts[i]; break;
        }
    }
    free(fmts);

    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && want > caps.maxImageCount) want = caps.maxImageCount;
    if (want > RHI_MAX_SC_IMAGES) want = RHI_MAX_SC_IMAGES;

    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = dev->surface,
        .minImageCount = want,
        .imageFormat = pick.format,
        .imageColorSpace = pick.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        /* COLOR_ATTACHMENT for generality; TRANSFER_DST because we blit
         * the offscreen target onto it. */
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = choose_present_mode(dev), /* FIFO (vsync) unless OPENPETE_PRESENT_MODE */
        .clipped = VK_TRUE,
        .oldSwapchain = VK_NULL_HANDLE,
    };
    VkResult r = vkCreateSwapchainKHR(dev->device, &sci, NULL, &dev->swapchain);
    if (r != VK_SUCCESS) { RHI_LOG("vkCreateSwapchainKHR failed: %d\n", (int)r); return false; }

    dev->sc_format = pick.format;
    dev->sc_extent = extent;
    dev->sc_count = 0;
    vkGetSwapchainImagesKHR(dev->device, dev->swapchain, &dev->sc_count, NULL);
    if (dev->sc_count > RHI_MAX_SC_IMAGES) dev->sc_count = RHI_MAX_SC_IMAGES;
    vkGetSwapchainImagesKHR(dev->device, dev->swapchain, &dev->sc_count, dev->sc_images);

    for (uint32_t i = 0; i < RHI_MAX_SC_IMAGES; i++)
        dev->images_in_flight[i] = VK_NULL_HANDLE;
    return true;
}

static bool create_render_pass(rhi_device *dev)
{
    VkAttachmentDescription color = {
        .format = dev->sc_format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        /* Leave it ready for the blit that reads it after the pass. */
        .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    };
    VkAttachmentDescription depth = {
        .format = VK_FORMAT_D32_SFLOAT,   /* required-supported depth format */
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, /* not read after the pass */
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference depth_ref = {
        .attachment = 1,
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &ref,
        .pDepthStencilAttachment = &depth_ref,
    };
    VkSubpassDependency deps[3] = {
        { /* external -> subpass: acquire color + depth attachments */
            .srcSubpass = VK_SUBPASS_EXTERNAL,
            .dstSubpass = 0,
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        },
        { /* subpass -> external: make color writes available to the blit */
            .srcSubpass = 0,
            .dstSubpass = VK_SUBPASS_EXTERNAL,
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        },
        { /* external -> subpass (WAR): off_image is a SINGLE shared target, so
           * the next frame's color write must wait for the PREVIOUS frame's
           * transfer reads of it — the blit AND the async --record copy
           * (rhi_cycle_readback) — to finish, or an in-flight frame would
           * overwrite pixels still being read back. (The old blocking record
           * path's vkDeviceWaitIdle masked this overlap; the async ring needs it
           * explicit so recordings stay tear-free.) WAR ⇒ execution dependency. */
            .srcSubpass = VK_SUBPASS_EXTERNAL,
            .dstSubpass = 0,
            .srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        },
    };
    VkAttachmentDescription atts[2] = { color, depth };
    VkRenderPassCreateInfo rci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2,
        .pAttachments = atts,
        .subpassCount = 1,
        .pSubpasses = &sub,
        .dependencyCount = 3,
        .pDependencies = deps,
    };
    VkResult r = vkCreateRenderPass(dev->device, &rci, NULL, &dev->off_rp);
    if (r != VK_SUCCESS) { RHI_LOG("vkCreateRenderPass failed: %d\n", (int)r); return false; }
    return true;
}

/* Render pass for render-to-texture targets. Identical attachment FORMATS to
 * off_rp (so pipelines built against off_rp are compatible and can be used
 * here), but the color attachment ends in SHADER_READ_ONLY_OPTIMAL — i.e. the
 * rendered image is immediately sampleable by a later pass in the same frame. */
static bool create_rt_render_pass(rhi_device *dev)
{
    VkAttachmentDescription color = {
        .format = dev->sc_format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkAttachmentDescription depth = {
        .format = VK_FORMAT_D32_SFLOAT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference ref = { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth_ref = { .attachment = 1, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &ref,
        .pDepthStencilAttachment = &depth_ref,
    };
    VkSubpassDependency deps[2] = {
        { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
          .srcAccessMask = 0,
          .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT },
        { /* subpass -> external: make the color result available to the sampler */
          .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT },
    };
    VkAttachmentDescription atts[2] = { color, depth };
    VkRenderPassCreateInfo rci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2, .pAttachments = atts,
        .subpassCount = 1, .pSubpasses = &sub,
        .dependencyCount = 2, .pDependencies = deps,
    };
    VkResult r = vkCreateRenderPass(dev->device, &rci, NULL, &dev->rt_rp);
    if (r != VK_SUCCESS) { RHI_LOG("vkCreateRenderPass(rt) failed: %d\n", (int)r); return false; }
    return true;
}

static bool create_offscreen(rhi_device *dev)
{
    /* The offscreen target matches the swapchain extent. The 3D scene composites
     * here and rhi_blit_to_swapchain presents it 1:1. SSAA renders the scene into
     * a larger render target (renderer-owned) and resolves it into this. */
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = dev->sc_format,
        .extent = { dev->sc_extent.width, dev->sc_extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(dev->device, &ici, NULL, &dev->off_image) != VK_SUCCESS) {
        RHI_LOG("offscreen vkCreateImage failed\n");
        return false;
    }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev->device, dev->off_image, &mr);
    uint32_t mt = find_memtype(dev->phys, mr.memoryTypeBits,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) { RHI_LOG("no device-local memory type\n"); return false; }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = mt,
    };
    if (vkAllocateMemory(dev->device, &mai, NULL, &dev->off_mem) != VK_SUCCESS) {
        RHI_LOG("offscreen vkAllocateMemory failed\n");
        return false;
    }
    vkBindImageMemory(dev->device, dev->off_image, dev->off_mem, 0);

    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = dev->off_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = dev->sc_format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1, .layerCount = 1,
        },
    };
    if (vkCreateImageView(dev->device, &vci, NULL, &dev->off_view) != VK_SUCCESS) {
        RHI_LOG("offscreen vkCreateImageView failed\n");
        return false;
    }
    /* Depth attachment (3a.3): D32_SFLOAT, device-local, depth-stencil usage. */
    VkImageCreateInfo dici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT,
        .extent = { dev->sc_extent.width, dev->sc_extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(dev->device, &dici, NULL, &dev->off_depth_image) != VK_SUCCESS) {
        RHI_LOG("offscreen depth vkCreateImage failed\n");
        return false;
    }
    VkMemoryRequirements dmr;
    vkGetImageMemoryRequirements(dev->device, dev->off_depth_image, &dmr);
    uint32_t dmt = find_memtype(dev->phys, dmr.memoryTypeBits,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (dmt == UINT32_MAX) { RHI_LOG("no device-local memory type (depth)\n"); return false; }
    VkMemoryAllocateInfo dmai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = dmr.size,
        .memoryTypeIndex = dmt,
    };
    if (vkAllocateMemory(dev->device, &dmai, NULL, &dev->off_depth_mem) != VK_SUCCESS) {
        RHI_LOG("offscreen depth vkAllocateMemory failed\n");
        return false;
    }
    vkBindImageMemory(dev->device, dev->off_depth_image, dev->off_depth_mem, 0);
    VkImageViewCreateInfo dvci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = dev->off_depth_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .levelCount = 1, .layerCount = 1,
        },
    };
    if (vkCreateImageView(dev->device, &dvci, NULL, &dev->off_depth_view) != VK_SUCCESS) {
        RHI_LOG("offscreen depth vkCreateImageView failed\n");
        return false;
    }

    VkImageView fb_atts[2] = { dev->off_view, dev->off_depth_view };
    VkFramebufferCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = dev->off_rp,
        .attachmentCount = 2,
        .pAttachments = fb_atts,
        .width = dev->sc_extent.width,
        .height = dev->sc_extent.height,
        .layers = 1,
    };
    if (vkCreateFramebuffer(dev->device, &fci, NULL, &dev->off_fb) != VK_SUCCESS) {
        RHI_LOG("offscreen vkCreateFramebuffer failed\n");
        return false;
    }
    return true;
}

static bool create_sync_and_commands(rhi_device *dev)
{
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = dev->gfx_family,
    };
    if (vkCreateCommandPool(dev->device, &pci, NULL, &dev->pool) != VK_SUCCESS) {
        RHI_LOG("vkCreateCommandPool failed\n");
        return false;
    }
    VkCommandBufferAllocateInfo cbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = dev->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = RHI_FRAMES_IN_FLIGHT,
    };
    if (vkAllocateCommandBuffers(dev->device, &cbi, dev->cmd) != VK_SUCCESS) {
        RHI_LOG("vkAllocateCommandBuffers failed\n");
        return false;
    }
    VkSemaphoreCreateInfo sem = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fence = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    for (int i = 0; i < RHI_FRAMES_IN_FLIGHT; i++) {
        VK_CHECK(vkCreateSemaphore(dev->device, &sem, NULL, &dev->img_avail[i]));
        VK_CHECK(vkCreateFence(dev->device, &fence, NULL, &dev->in_flight[i]));
    }
    for (uint32_t i = 0; i < dev->sc_count; i++) {
        VK_CHECK(vkCreateSemaphore(dev->device, &sem, NULL, &dev->render_done[i]));
    }
    return true;
}

/* The VRAM sampler is NEAREST + integer format (R16_UINT): the PSX sampler
 * does its own texelFetch indexing, so no GPU filtering. The set layout is a
 * single combined-image-sampler at set 0 binding 0, shared by every
 * VRAM-sampling pipeline; each texture gets its own set from the pool. */
static bool create_descriptor_infra(rhi_device *dev)
{
    VkSamplerCreateInfo si = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0.0f,
    };
    if (vkCreateSampler(dev->device, &si, NULL, &dev->vram_sampler) != VK_SUCCESS) {
        RHI_LOG("vkCreateSampler failed\n"); return false;
    }
    VkDescriptorSetLayoutBinding b = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo li = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &b,
    };
    if (vkCreateDescriptorSetLayout(dev->device, &li, NULL, &dev->vram_set_layout) != VK_SUCCESS) {
        RHI_LOG("vkCreateDescriptorSetLayout failed\n"); return false;
    }
    VkDescriptorPoolSize ps = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 16,
    };
    VkDescriptorPoolCreateInfo pi = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 16, .poolSizeCount = 1, .pPoolSizes = &ps,
    };
    if (vkCreateDescriptorPool(dev->device, &pi, NULL, &dev->desc_pool) != VK_SUCCESS) {
        RHI_LOG("vkCreateDescriptorPool failed\n"); return false;
    }
    return true;
}

/* A transient command buffer for one-off transfers (VRAM uploads, initial
 * image transitions). Submits and blocks; updates are occasional so the
 * stall is acceptable — the per-frame path never touches this. */
static VkCommandBuffer one_shot_begin(rhi_device *dev)
{
    VkCommandBufferAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = dev->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(dev->device, &ai, &cb) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

static void one_shot_end(rhi_device *dev, VkCommandBuffer cb)
{
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cb,
    };
    VK_CHECK(vkQueueSubmit(dev->gfx_queue, 1, &si, VK_NULL_HANDLE));
    vkQueueWaitIdle(dev->gfx_queue);
    vkFreeCommandBuffers(dev->device, dev->pool, 1, &cb);
}

static void destroy_swapchain_resources(rhi_device *dev)
{
    if (dev->off_fb)    { vkDestroyFramebuffer(dev->device, dev->off_fb, NULL); dev->off_fb = VK_NULL_HANDLE; }
    if (dev->off_view)  { vkDestroyImageView(dev->device, dev->off_view, NULL); dev->off_view = VK_NULL_HANDLE; }
    if (dev->off_image) { vkDestroyImage(dev->device, dev->off_image, NULL); dev->off_image = VK_NULL_HANDLE; }
    if (dev->off_mem)   { vkFreeMemory(dev->device, dev->off_mem, NULL); dev->off_mem = VK_NULL_HANDLE; }
    if (dev->off_depth_view)  { vkDestroyImageView(dev->device, dev->off_depth_view, NULL); dev->off_depth_view = VK_NULL_HANDLE; }
    if (dev->off_depth_image) { vkDestroyImage(dev->device, dev->off_depth_image, NULL); dev->off_depth_image = VK_NULL_HANDLE; }
    if (dev->off_depth_mem)   { vkFreeMemory(dev->device, dev->off_depth_mem, NULL); dev->off_depth_mem = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < RHI_MAX_SC_IMAGES; i++) {
        if (dev->render_done[i]) {
            vkDestroySemaphore(dev->device, dev->render_done[i], NULL);
            dev->render_done[i] = VK_NULL_HANDLE;
        }
    }
    if (dev->swapchain) { vkDestroySwapchainKHR(dev->device, dev->swapchain, NULL); dev->swapchain = VK_NULL_HANDLE; }
}

static bool recreate(rhi_device *dev)
{
    vkDeviceWaitIdle(dev->device);
    destroy_swapchain_resources(dev);
    if (!create_swapchain(dev)) return false;
    if (!create_offscreen(dev)) return false;
    VkSemaphoreCreateInfo sem = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < dev->sc_count; i++)
        VK_CHECK(vkCreateSemaphore(dev->device, &sem, NULL, &dev->render_done[i]));
    dev->needs_recreate = false;
    return true;
}

/* ------------------------------------------------------------------ */

static rhi_device *rhi_create_impl(struct SDL_Window *window, const char *app_name,
                                   bool headless, uint32_t hl_w, uint32_t hl_h)
{
    rhi_device *dev = calloc(1, sizeof(*dev));
    if (!dev) return NULL;
    dev->window   = window;
    dev->headless = headless;

    /* Dynamically load the Vulkan loader (libvulkan.so.1 / vulkan-1.dll);
     * no loader import library is linked on any platform. */
    if (volkInitialize() != VK_SUCCESS) {
        RHI_LOG("volkInitialize failed — no Vulkan loader found\n");
        free(dev);
        return NULL;
    }

    if (!create_instance(dev, app_name)) goto fail;
    volkLoadInstance(dev->instance);   /* load instance- + device-level fns */

    if (!headless) {
        if (!SDL_Vulkan_CreateSurface(window, dev->instance, NULL, &dev->surface)) {
            RHI_LOG("SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
            goto fail;
        }
    }
    if (!pick_physical_and_device(dev)) goto fail;
    volkLoadDevice(dev->device);        /* device-specific dispatch (faster) */
    if (!headless) {
        if (!create_swapchain(dev)) goto fail;
    } else {
        /* No swapchain: the offscreen render pass + offscreen image still need a
         * format + extent. Pick B8G8R8A8_UNORM (matches the windowed preference,
         * so rhi_read_render_target's B/R swizzle is correct) and the requested
         * size. sc_count stays 0 → no render_done semaphores, no present. */
        dev->sc_format = VK_FORMAT_B8G8R8A8_UNORM;
        dev->sc_extent = (VkExtent2D){ hl_w ? hl_w : 1u, hl_h ? hl_h : 1u };
        dev->sc_count  = 0;
        for (uint32_t i = 0; i < RHI_MAX_SC_IMAGES; i++)
            dev->images_in_flight[i] = VK_NULL_HANDLE;
    }
    if (!create_render_pass(dev))       goto fail;
    if (!create_rt_render_pass(dev))    goto fail;
    if (!create_offscreen(dev))         goto fail;
    if (!create_sync_and_commands(dev)) goto fail;
    if (!create_descriptor_infra(dev))  goto fail;

    RHI_LOG("device='%s'%s format=%d %ux%u images=%u\n",
            dev->device_name, headless ? " (headless)" : "", (int)dev->sc_format,
            dev->sc_extent.width, dev->sc_extent.height, dev->sc_count);
    return dev;

fail:
    rhi_destroy(dev);
    return NULL;
}

rhi_device *rhi_create(struct SDL_Window *window, const char *app_name)
{
    return rhi_create_impl(window, app_name, /*headless=*/false, 0, 0);
}

rhi_device *rhi_create_headless(const char *app_name, uint32_t width, uint32_t height)
{
    return rhi_create_impl(NULL, app_name, /*headless=*/true, width, height);
}

bool rhi_is_headless(const rhi_device *dev)
{
    return dev && dev->headless;
}

void rhi_destroy(rhi_device *dev)
{
    if (!dev) return;
    if (dev->device) vkDeviceWaitIdle(dev->device);

    if (dev->readback_buf) { rhi_destroy_buffer(dev, dev->readback_buf); dev->readback_buf = NULL; }
    for (int i = 0; i < RHI_FRAMES_IN_FLIGHT; i++) {
        if (dev->readback_ring[i].staging) {
            rhi_destroy_buffer(dev, dev->readback_ring[i].staging);
            dev->readback_ring[i].staging = NULL;
        }
    }
    destroy_swapchain_resources(dev);
    if (dev->off_rp) vkDestroyRenderPass(dev->device, dev->off_rp, NULL);
    if (dev->rt_rp)  vkDestroyRenderPass(dev->device, dev->rt_rp, NULL);

    if (dev->desc_pool)      vkDestroyDescriptorPool(dev->device, dev->desc_pool, NULL);
    if (dev->vram_set_layout) vkDestroyDescriptorSetLayout(dev->device, dev->vram_set_layout, NULL);
    if (dev->vram_sampler)   vkDestroySampler(dev->device, dev->vram_sampler, NULL);

    for (int i = 0; i < RHI_FRAMES_IN_FLIGHT; i++) {
        if (dev->img_avail[i]) vkDestroySemaphore(dev->device, dev->img_avail[i], NULL);
        if (dev->in_flight[i]) vkDestroyFence(dev->device, dev->in_flight[i], NULL);
    }
    if (dev->pool)   vkDestroyCommandPool(dev->device, dev->pool, NULL);
    if (dev->device) vkDestroyDevice(dev->device, NULL);
    if (dev->surface) vkDestroySurfaceKHR(dev->instance, dev->surface, NULL);
    if (dev->debug) {
        PFN_vkDestroyDebugUtilsMessengerEXT rm =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                dev->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (rm) rm(dev->instance, dev->debug, NULL);
    }
    if (dev->instance) vkDestroyInstance(dev->instance, NULL);
    free(dev);
}

void rhi_wait_idle(rhi_device *dev)
{
    if (dev && dev->device) vkDeviceWaitIdle(dev->device);
}

bool rhi_begin_frame(rhi_device *dev)
{
    if (dev->needs_recreate && !dev->headless) {
        if (!recreate(dev)) return false;
    }

    vkWaitForFences(dev->device, 1, &dev->in_flight[dev->frame], VK_TRUE, UINT64_MAX);

    /* Headless: no swapchain image to acquire — the command buffer records
     * directly into the offscreen / render-target images. */
    if (!dev->headless) {
        VkResult acq = vkAcquireNextImageKHR(dev->device, dev->swapchain, UINT64_MAX,
                                             dev->img_avail[dev->frame],
                                             VK_NULL_HANDLE, &dev->image_index);
        if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
            dev->needs_recreate = true;
            return false;
        } else if (acq == VK_ERROR_DEVICE_LOST) {
            rhi_signal_device_lost("acquire");
            return false;
        } else if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
            RHI_LOG("vkAcquireNextImageKHR failed: %d\n", (int)acq);
            return false;
        }

        /* If a previous frame is still using this image, wait on its fence. */
        if (dev->images_in_flight[dev->image_index] != VK_NULL_HANDLE) {
            vkWaitForFences(dev->device, 1, &dev->images_in_flight[dev->image_index],
                            VK_TRUE, UINT64_MAX);
        }
        dev->images_in_flight[dev->image_index] = dev->in_flight[dev->frame];
    }
    vkResetFences(dev->device, 1, &dev->in_flight[dev->frame]);

    VkCommandBuffer cb = dev->cmd[dev->frame];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cb, &bi);
    dev->frame_active = true;
    return true;
}

void rhi_begin_offscreen(rhi_device *dev, float r, float g, float b, float a)
{
    if (!dev->frame_active) return;
    dev->clear[0] = r; dev->clear[1] = g; dev->clear[2] = b; dev->clear[3] = a;
    VkClearValue cv[2];
    cv[0].color.float32[0] = r; cv[0].color.float32[1] = g;
    cv[0].color.float32[2] = b; cv[0].color.float32[3] = a;
    cv[1].depthStencil.depth = 1.0f; cv[1].depthStencil.stencil = 0; /* far */
    VkRenderPassBeginInfo rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = dev->off_rp,
        .framebuffer = dev->off_fb,
        .renderArea = { .offset = {0, 0}, .extent = dev->sc_extent },
        .clearValueCount = 2,
        .pClearValues = cv,
    };
    vkCmdBeginRenderPass(dev->cmd[dev->frame], &rp, VK_SUBPASS_CONTENTS_INLINE);
    dev->cur_extent = dev->sc_extent;   /* render area of this (offscreen) pass */

    /* Y-flipped viewport (Vulkan clip-space Y is down) so NDC/UV match the
     * GL/texture convention the ported PSX shaders assume. Dynamic so a
     * resize needs no pipeline rebuild. */
    VkViewport vp = {
        .x = 0.0f,
        .y = (float)dev->sc_extent.height,
        .width = (float)dev->sc_extent.width,
        .height = -(float)dev->sc_extent.height,
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };
    VkRect2D scissor = { .offset = {0, 0}, .extent = dev->sc_extent };
    vkCmdSetViewport(dev->cmd[dev->frame], 0, 1, &vp);
    vkCmdSetScissor(dev->cmd[dev->frame], 0, 1, &scissor);
}

void rhi_end_offscreen(rhi_device *dev)
{
    if (!dev->frame_active) return;
    vkCmdEndRenderPass(dev->cmd[dev->frame]);
}

static void image_barrier(VkCommandBuffer cb, VkImage img,
                          VkImageLayout from, VkImageLayout to,
                          VkAccessFlags src_access, VkAccessFlags dst_access,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1, .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

void rhi_blit_to_swapchain(rhi_device *dev)
{
    if (!dev->frame_active || dev->headless) return;   /* no swapchain when headless */
    VkCommandBuffer cb = dev->cmd[dev->frame];
    VkImage sc = dev->sc_images[dev->image_index];

    /* Swapchain image: undefined -> transfer dst. (Offscreen is already
     * TRANSFER_SRC_OPTIMAL via the render pass final layout.) */
    image_barrier(cb, sc, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageBlit region = {
        .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .srcOffsets = { {0,0,0}, {(int32_t)dev->sc_extent.width, (int32_t)dev->sc_extent.height, 1} },
        .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .dstOffsets = { {0,0,0}, {(int32_t)dev->sc_extent.width, (int32_t)dev->sc_extent.height, 1} },
    };
    vkCmdBlitImage(cb, dev->off_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);

    /* Swapchain image: transfer dst -> present. */
    image_barrier(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

void rhi_snapshot_offscreen_to_rt(rhi_device *dev, rhi_texture *dst,
                                  int sx, int sy, int sw, int sh)
{
    if (!dev->frame_active || !dst || !dst->is_rt) return;
    VkCommandBuffer cb = dev->cmd[dev->frame];
    /* off_image is TRANSFER_SRC (offscreen render-pass final layout). Flip dst
     * from shader-read to transfer-dst, hardware-blit the source region (scaled
     * to fill dst), then flip dst back to shader-read so it stays sampleable. */
    image_barrier(cb, dst->image, dst->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit region = {
        .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .srcOffsets = { { sx, sy, 0 }, { sx + sw, sy + sh, 1 } },
        .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .dstOffsets = { { 0, 0, 0 }, { (int32_t)dst->width, (int32_t)dst->height, 1 } },
    };
    vkCmdBlitImage(cb, dev->off_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
    image_barrier(cb, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    dst->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void rhi_end_frame(rhi_device *dev)
{
    if (!dev->frame_active) return;
    VkCommandBuffer cb = dev->cmd[dev->frame];
    vkEndCommandBuffer(cb);

    /* Headless: submit the recorded work with no swapchain semaphores and no
     * present. The in_flight fence still gates begin_frame's reuse of this slot,
     * and rhi_read_render_target's vkDeviceWaitIdle guarantees completion before
     * readback. */
    if (dev->headless) {
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &cb,
        };
        VK_CHECK(vkQueueSubmit(dev->gfx_queue, 1, &si, dev->in_flight[dev->frame]));
        dev->frame = (dev->frame + 1) % RHI_FRAMES_IN_FLIGHT;
        dev->frame_active = false;
        return;
    }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &dev->img_avail[dev->frame],
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &dev->render_done[dev->image_index],
    };
    VK_CHECK(vkQueueSubmit(dev->gfx_queue, 1, &si, dev->in_flight[dev->frame]));

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &dev->render_done[dev->image_index],
        .swapchainCount = 1,
        .pSwapchains = &dev->swapchain,
        .pImageIndices = &dev->image_index,
    };
    VkResult pr = vkQueuePresentKHR(dev->gfx_queue, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        dev->needs_recreate = true;
    } else if (pr == VK_ERROR_DEVICE_LOST) {
        rhi_signal_device_lost("present");
    } else if (pr != VK_SUCCESS) {
        RHI_LOG("vkQueuePresentKHR failed: %d\n", (int)pr);
    }

    dev->frame = (dev->frame + 1) % RHI_FRAMES_IN_FLIGHT;
    dev->frame_active = false;
}

void rhi_notify_resize(rhi_device *dev)
{
    if (dev) dev->needs_recreate = true;
}

uint32_t rhi_max_image_dimension(const rhi_device *dev)
{
    if (!dev) return 0;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(dev->phys, &props);
    return props.limits.maxImageDimension2D;
}

void rhi_texture_extent(const rhi_texture *tex, int *w, int *h)
{
    if (w) *w = tex ? (int)tex->width  : 0;
    if (h) *h = tex ? (int)tex->height : 0;
}

const char *rhi_device_name(const rhi_device *dev)
{
    return dev ? dev->device_name : "";
}

/* ------------------------------------------------------------------ */
/* Buffers                                                            */
/* ------------------------------------------------------------------ */

/* Host-visible, coherent, persistently-mapped buffer with the given usage.
 * prefer_cached: also request HOST_CACHED (falling back to plain coherent if no
 * such type exists). Set it for GPU->CPU READBACK buffers — the CPU reads them
 * back, and on discrete GPUs the default host-visible|coherent type is
 * write-combined, where CPU READS are glacially slow (tens of ms for a frame's
 * worth of pixels — that, not the device-wait, is what capped --record). Leave
 * it false for CPU->GPU upload buffers (vertex/staging), which the CPU only
 * writes (write-combined is ideal there). Coherent in both cases ⇒ no manual
 * flush/invalidate around the mapping. */
static rhi_buffer *create_host_buffer(rhi_device *dev, size_t bytes,
                                      VkBufferUsageFlags usage, bool prefer_cached)
{
    rhi_buffer *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->size = (VkDeviceSize)bytes;

    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(dev->device, &bi, NULL, &b->buffer) != VK_SUCCESS) {
        RHI_LOG("vkCreateBuffer failed\n"); free(b); return NULL;
    }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev->device, b->buffer, &mr);
    const VkMemoryPropertyFlags base = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t mt = UINT32_MAX;
    if (prefer_cached) {
        mt = find_memtype(dev->phys, mr.memoryTypeBits,
                          base | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        RHI_LOG("readback buffer (%zu bytes): %s\n", bytes,
                mt != UINT32_MAX ? "HOST_CACHED (fast CPU read)"
                                 : "no HOST_CACHED type — falling back to "
                                   "write-combined (slow CPU read)");
    }
    if (mt == UINT32_MAX)
        mt = find_memtype(dev->phys, mr.memoryTypeBits, base);
    if (mt == UINT32_MAX) {
        RHI_LOG("no host-visible memory type\n");
        vkDestroyBuffer(dev->device, b->buffer, NULL); free(b); return NULL;
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = mt,
    };
    if (vkAllocateMemory(dev->device, &mai, NULL, &b->memory) != VK_SUCCESS) {
        RHI_LOG("vkAllocateMemory (buffer) failed\n");
        vkDestroyBuffer(dev->device, b->buffer, NULL); free(b); return NULL;
    }
    vkBindBufferMemory(dev->device, b->buffer, b->memory, 0);
    vkMapMemory(dev->device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->mapped);
    return b;
}

rhi_buffer *rhi_create_vertex_buffer(rhi_device *dev, size_t bytes)
{
    return create_host_buffer(dev, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              /*prefer_cached=*/false);
}

void rhi_update_buffer(rhi_buffer *buf, const void *data, size_t bytes)
{
    if (!buf || !buf->mapped) return;
    if ((VkDeviceSize)bytes > buf->size) bytes = (size_t)buf->size;
    memcpy(buf->mapped, data, bytes);
}

void rhi_destroy_buffer(rhi_device *dev, rhi_buffer *buf)
{
    if (!buf) return;
    if (buf->memory) {
        vkUnmapMemory(dev->device, buf->memory);
        vkFreeMemory(dev->device, buf->memory, NULL);
    }
    if (buf->buffer) vkDestroyBuffer(dev->device, buf->buffer, NULL);
    free(buf);
}

/* ------------------------------------------------------------------ */
/* Sampled textures: the R16_UINT VRAM model + RGBA8 (frame readback)  */
/* ------------------------------------------------------------------ */

/* Create a sampled 2D image of `fmt` (bpp bytes/pixel) plus a host-visible
 * staging buffer and a descriptor set bound to (view, shared NEAREST
 * sampler). The descriptor layout is format-agnostic — one combined image
 * sampler at set 0, binding 0 — so it serves both the VRAM (usampler2D) and
 * the RGBA8 (sampler2D) shaders. */
static rhi_texture *create_sampled_image(rhi_device *dev, uint32_t width,
                                         uint32_t height, VkFormat fmt,
                                         uint32_t bpp)
{
    rhi_texture *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->width = width; t->height = height; t->bpp = bpp;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt,
        .extent = { width, height, 1 },
        .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(dev->device, &ici, NULL, &t->image) != VK_SUCCESS) {
        RHI_LOG("vram vkCreateImage failed\n"); goto fail;
    }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev->device, t->image, &mr);
    uint32_t mt = find_memtype(dev->phys, mr.memoryTypeBits,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) { RHI_LOG("vram: no device-local memory\n"); goto fail; }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = mt,
    };
    if (vkAllocateMemory(dev->device, &mai, NULL, &t->mem) != VK_SUCCESS) {
        RHI_LOG("vram vkAllocateMemory failed\n"); goto fail;
    }
    vkBindImageMemory(dev->device, t->image, t->mem, 0);

    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = t->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1, .layerCount = 1,
        },
    };
    if (vkCreateImageView(dev->device, &vci, NULL, &t->view) != VK_SUCCESS) {
        RHI_LOG("vram vkCreateImageView failed\n"); goto fail;
    }

    t->staging = create_host_buffer(dev, (size_t)width * height * bpp,
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                    /*prefer_cached=*/false);
    if (!t->staging) { RHI_LOG("staging alloc failed\n"); goto fail; }

    /* Allocate + point a descriptor set at (this view, the shared sampler). */
    VkDescriptorSetAllocateInfo dsi = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dev->desc_pool,
        .descriptorSetCount = 1, .pSetLayouts = &dev->vram_set_layout,
    };
    if (vkAllocateDescriptorSets(dev->device, &dsi, &t->set) != VK_SUCCESS) {
        RHI_LOG("vram vkAllocateDescriptorSets failed\n"); goto fail;
    }
    VkDescriptorImageInfo dii = {
        .sampler = dev->vram_sampler,
        .imageView = t->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = t->set, .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &dii,
    };
    vkUpdateDescriptorSets(dev->device, 1, &w, 0, NULL);

    /* Transition to SHADER_READ_ONLY so the texture is always in a valid
     * sampling layout, even before the first upload. */
    VkCommandBuffer cb = one_shot_begin(dev);
    image_barrier(cb, t->image, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  0, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    one_shot_end(dev, cb);
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return t;

fail:
    rhi_destroy_texture(dev, t);
    return NULL;
}

rhi_texture *rhi_create_vram(rhi_device *dev, uint32_t width, uint32_t height)
{
    /* R16_UINT: one uint per PSX VRAM pixel, sampled via texelFetch (the PSX
     * shader does its own integer indexing — no GPU filtering/normalization). */
    return create_sampled_image(dev, width, height, VK_FORMAT_R16_UINT, 2);
}

rhi_texture *rhi_create_rgba8(rhi_device *dev, uint32_t width, uint32_t height)
{
    /* R8G8B8A8_UNORM: bytes laid out R,G,B,A — matches a GL_RGBA readback;
     * sampled as a normal sampler2D (e.g. another renderer's frame for A/B). */
    return create_sampled_image(dev, width, height, VK_FORMAT_R8G8B8A8_UNORM, 4);
}

rhi_texture *rhi_create_render_target(rhi_device *dev, uint32_t width, uint32_t height)
{
    rhi_texture *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->is_rt = true;
    t->width = width; t->height = height; t->bpp = 4;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;

    /* Color image: same format as the offscreen/swapchain (so rt_rp & off_rp
     * pipelines are compatible), usable as a color attachment AND a sampler. */
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = dev->sc_format,
        .extent = { width, height, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(dev->device, &ici, NULL, &t->image) != VK_SUCCESS) {
        RHI_LOG("rt vkCreateImage failed\n"); goto fail;
    }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev->device, t->image, &mr);
    uint32_t mt = find_memtype(dev->phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) { RHI_LOG("rt: no device-local memory\n"); goto fail; }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = mr.size, .memoryTypeIndex = mt };
    if (vkAllocateMemory(dev->device, &mai, NULL, &t->mem) != VK_SUCCESS) {
        RHI_LOG("rt vkAllocateMemory failed\n"); goto fail;
    }
    vkBindImageMemory(dev->device, t->image, t->mem, 0);
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = dev->sc_format,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
    };
    if (vkCreateImageView(dev->device, &vci, NULL, &t->view) != VK_SUCCESS) {
        RHI_LOG("rt vkCreateImageView failed\n"); goto fail;
    }

    /* Depth attachment for the RT framebuffer. */
    VkImageCreateInfo dici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT,
        .extent = { width, height, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(dev->device, &dici, NULL, &t->rt_depth_image) != VK_SUCCESS) {
        RHI_LOG("rt depth vkCreateImage failed\n"); goto fail;
    }
    VkMemoryRequirements dmr;
    vkGetImageMemoryRequirements(dev->device, t->rt_depth_image, &dmr);
    uint32_t dmt = find_memtype(dev->phys, dmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (dmt == UINT32_MAX) { RHI_LOG("rt: no device-local memory (depth)\n"); goto fail; }
    VkMemoryAllocateInfo dmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .allocationSize = dmr.size, .memoryTypeIndex = dmt };
    if (vkAllocateMemory(dev->device, &dmai, NULL, &t->rt_depth_mem) != VK_SUCCESS) {
        RHI_LOG("rt depth vkAllocateMemory failed\n"); goto fail;
    }
    vkBindImageMemory(dev->device, t->rt_depth_image, t->rt_depth_mem, 0);
    VkImageViewCreateInfo dvci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->rt_depth_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .levelCount = 1, .layerCount = 1 },
    };
    if (vkCreateImageView(dev->device, &dvci, NULL, &t->rt_depth_view) != VK_SUCCESS) {
        RHI_LOG("rt depth vkCreateImageView failed\n"); goto fail;
    }
    VkImageView fb_atts[2] = { t->view, t->rt_depth_view };
    VkFramebufferCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = dev->rt_rp,
        .attachmentCount = 2, .pAttachments = fb_atts,
        .width = width, .height = height, .layers = 1,
    };
    if (vkCreateFramebuffer(dev->device, &fci, NULL, &t->rt_fb) != VK_SUCCESS) {
        RHI_LOG("rt vkCreateFramebuffer failed\n"); goto fail;
    }

    /* Descriptor set (this view + shared sampler) so it binds like any texture. */
    VkDescriptorSetAllocateInfo dsi = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dev->desc_pool, .descriptorSetCount = 1, .pSetLayouts = &dev->vram_set_layout,
    };
    if (vkAllocateDescriptorSets(dev->device, &dsi, &t->set) != VK_SUCCESS) {
        RHI_LOG("rt vkAllocateDescriptorSets failed\n"); goto fail;
    }
    VkDescriptorImageInfo dii = {
        .sampler = dev->vram_sampler, .imageView = t->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = t->set, .dstBinding = 0,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii,
    };
    vkUpdateDescriptorSets(dev->device, 1, &w, 0, NULL);

    /* Valid sampling layout before the first render (defensive). */
    VkCommandBuffer cb = one_shot_begin(dev);
    image_barrier(cb, t->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  0, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    one_shot_end(dev, cb);
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return t;

fail:
    rhi_destroy_texture(dev, t);
    return NULL;
}

void rhi_begin_render_target(rhi_device *dev, rhi_texture *rt,
                             float r, float g, float b, float a)
{
    if (!dev->frame_active || !rt || !rt->is_rt) return;
    VkClearValue cv[2];
    cv[0].color.float32[0] = r; cv[0].color.float32[1] = g;
    cv[0].color.float32[2] = b; cv[0].color.float32[3] = a;
    cv[1].depthStencil.depth = 1.0f; cv[1].depthStencil.stencil = 0;
    VkExtent2D ext = { rt->width, rt->height };
    VkRenderPassBeginInfo rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = dev->rt_rp, .framebuffer = rt->rt_fb,
        .renderArea = { .offset = {0, 0}, .extent = ext },
        .clearValueCount = 2, .pClearValues = cv,
    };
    vkCmdBeginRenderPass(dev->cmd[dev->frame], &rp, VK_SUBPASS_CONTENTS_INLINE);
    dev->cur_extent = ext;   /* render area of this (render-target) pass */
    /* Y-flipped viewport, matching rhi_begin_offscreen. */
    VkViewport vp = { .x = 0.0f, .y = (float)rt->height,
                      .width = (float)rt->width, .height = -(float)rt->height,
                      .minDepth = 0.0f, .maxDepth = 1.0f };
    VkRect2D scissor = { .offset = {0, 0}, .extent = ext };
    vkCmdSetViewport(dev->cmd[dev->frame], 0, 1, &vp);
    vkCmdSetScissor(dev->cmd[dev->frame], 0, 1, &scissor);
}

void rhi_end_render_target(rhi_device *dev, rhi_texture *rt)
{
    if (!dev->frame_active || !rt || !rt->is_rt) return;
    vkCmdEndRenderPass(dev->cmd[dev->frame]);
    rt->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; /* rt_rp finalLayout */
}

/* Clear the depth attachment to far (1.0) WITHIN the current render pass — used
 * to isolate a late depth-tested overlay (the 3D HUD icons) from world depth so
 * it sorts internally yet always draws on top. vkCmdClearAttachments clears in
 * the active subpass over the given rect (no new render pass). */
void rhi_clear_depth(rhi_device *dev)
{
    if (!dev->frame_active) return;
    VkClearAttachment att = {
        .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
        .clearValue = { .depthStencil = { .depth = 1.0f, .stencil = 0 } },
    };
    VkClearRect rect = {
        .rect = { .offset = {0, 0}, .extent = dev->cur_extent },
        .baseArrayLayer = 0, .layerCount = 1,
    };
    vkCmdClearAttachments(dev->cmd[dev->frame], 1, &att, 1, &rect);
}

static void update_image(rhi_device *dev, rhi_texture *tex, const void *pixels)
{
    if (!tex || !tex->staging) return;
    memcpy(tex->staging->mapped, pixels,
           (size_t)tex->width * tex->height * tex->bpp);

    VkCommandBuffer cb = one_shot_begin(dev);
    image_barrier(cb, tex->image, tex->layout,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = {
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .imageExtent = { tex->width, tex->height, 1 },
    };
    vkCmdCopyBufferToImage(cb, tex->staging->buffer, tex->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    image_barrier(cb, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    one_shot_end(dev, cb);
    tex->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void rhi_update_vram(rhi_device *dev, rhi_texture *tex, const uint16_t *pixels)
{
    update_image(dev, tex, pixels);
}

void rhi_update_rgba8(rhi_device *dev, rhi_texture *tex, const void *pixels)
{
    update_image(dev, tex, pixels);
}

/* Copy a host-visible readback staging buffer (raw swapchain-format pixels) into
 * the caller's RGBA8 out buffer: swizzle B8G8R8A8 -> R,G,B,A and force opaque
 * alpha. Shared by the blocking screenshot path and the async --record ring. */
static void readback_swizzle(const rhi_device *dev, const void *src_, void *dst_,
                             int w, int h)
{
    const uint8_t *src = (const uint8_t *)src_;
    uint8_t *dst = (uint8_t *)dst_;
    int swap_rb = (dev->sc_format == VK_FORMAT_B8G8R8A8_UNORM ||
                   dev->sc_format == VK_FORMAT_B8G8R8A8_SRGB);
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        uint8_t c0 = src[i*4+0], c1 = src[i*4+1], c2 = src[i*4+2];
        dst[i*4+0] = swap_rb ? c2 : c0;
        dst[i*4+1] = c1;
        dst[i*4+2] = swap_rb ? c0 : c2;
        dst[i*4+3] = 0xFF;
    }
}

bool rhi_read_offscreen(rhi_device *dev, void *out, size_t out_bytes,
                        int *out_w, int *out_h)
{
    /* off_image is swapchain-sized and (for SSAA) already holds the resolved 1×
     * result, so the screenshot matches the present. */
    int w = (int)dev->sc_extent.width, h = (int)dev->sc_extent.height;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    size_t need = (size_t)w * (size_t)h * 4u;
    if (!out || out_bytes < need || w <= 0 || h <= 0) return false;

    /* The offscreen target is in TRANSFER_SRC_OPTIMAL after a rendered frame
     * (rhi_end_offscreen transitions it, the blit reads it as src). Wait for the
     * render to finish before copying — without a submission dependency the copy
     * could race the render's writes to off_image (queue submission order does not
     * guarantee completion order). Keep the device-wait for that correctness; the
     * big per-frame --record cost was the staging buffer allocated+freed EVERY
     * call, so reuse a persistent, grown-on-demand one instead. (A future async
     * readback ring removes the per-frame wait entirely for true real-speed.) */
    vkDeviceWaitIdle(dev->device);
    if (!dev->readback_buf || dev->readback_buf->size < need) {
        if (dev->readback_buf) rhi_destroy_buffer(dev, dev->readback_buf);
        dev->readback_buf = create_host_buffer(dev, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                               /*prefer_cached=*/true);
    }
    rhi_buffer *rb = dev->readback_buf;
    if (!rb) return false;

    VkCommandBuffer cb = one_shot_begin(dev);
    VkBufferImageCopy region = {
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .imageExtent = { (uint32_t)w, (uint32_t)h, 1 },
    };
    vkCmdCopyImageToBuffer(cb, dev->off_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           rb->buffer, 1, &region);
    one_shot_end(dev, cb);

    /* The swapchain/offscreen format is B8G8R8A8 (see the format pick); the
     * caller wants R,G,B,A. rb is dev->readback_buf — persistent; freed in
     * rhi_destroy. */
    readback_swizzle(dev, rb->mapped, out, w, h);
    return true;
}

/* ---- async frame readback ring (--record real-speed capture) ------------- */

bool rhi_cycle_readback(rhi_device *dev, uint64_t tag, void *out, size_t out_bytes,
                        int *out_w, int *out_h, uint64_t *out_tag)
{
    if (!dev || !dev->frame_active) return false;
    uint32_t fi = dev->frame;

    /* (1) Retrieve the capture queued the LAST time this slot was used. The copy
     * rode cmd[fi]/in_flight[fi]; rhi_begin_frame already waited on in_flight[fi]
     * this frame, so it has completed and its host-coherent staging is safe to
     * read with no extra GPU wait. */
    bool got = false;
    if (dev->readback_ring[fi].pending && dev->readback_ring[fi].staging &&
        dev->readback_ring[fi].w > 0 && dev->readback_ring[fi].h > 0) {
        int pw = dev->readback_ring[fi].w, ph = dev->readback_ring[fi].h;
        size_t need = (size_t)pw * (size_t)ph * 4u;
        if (out && out_bytes >= need) {
            readback_swizzle(dev, dev->readback_ring[fi].staging->mapped, out, pw, ph);
            if (out_w)   *out_w   = pw;
            if (out_h)   *out_h   = ph;
            if (out_tag) *out_tag = dev->readback_ring[fi].tag;
            got = true;
        }
        dev->readback_ring[fi].pending = false;   /* consumed (or dropped if out too small) */
    }

    /* (2) Queue THIS frame's copy into the slot. off_image is TRANSFER_SRC here
     * (render-pass final layout, already read by the blit); another transfer
     * read needs no barrier. The off_rp WAR dependency keeps the next frame from
     * overwriting off_image before this copy runs. */
    int w = (int)dev->sc_extent.width, h = (int)dev->sc_extent.height;
    if (w > 0 && h > 0) {
        size_t need = (size_t)w * (size_t)h * 4u;
        rhi_buffer **slot_staging = &dev->readback_ring[fi].staging;
        if (!*slot_staging || (*slot_staging)->size < need) {
            if (*slot_staging) rhi_destroy_buffer(dev, *slot_staging);
            *slot_staging = create_host_buffer(dev, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                               /*prefer_cached=*/true);
        }
        if (*slot_staging) {
            VkBufferImageCopy region = {
                .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
                .imageExtent = { (uint32_t)w, (uint32_t)h, 1 },
            };
            vkCmdCopyImageToBuffer(dev->cmd[fi], dev->off_image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   (*slot_staging)->buffer, 1, &region);
            dev->readback_ring[fi].pending = true;
            dev->readback_ring[fi].w = w;
            dev->readback_ring[fi].h = h;
            dev->readback_ring[fi].tag = tag;
        }
    }
    return got;
}

bool rhi_drain_readback(rhi_device *dev, void *out, size_t out_bytes,
                        int *out_w, int *out_h, uint64_t *out_tag)
{
    if (!dev) return false;
    /* Shutdown only: block until every queued copy is complete, then hand back
     * the still-pending slots oldest-first (by tag) so the final frames keep
     * monotonic PTS. Called ≤ RHI_FRAMES_IN_FLIGHT times; the wait is cheap once
     * the device is already idle. */
    vkDeviceWaitIdle(dev->device);
    int best = -1;
    for (int i = 0; i < RHI_FRAMES_IN_FLIGHT; i++) {
        if (dev->readback_ring[i].pending && dev->readback_ring[i].staging &&
            dev->readback_ring[i].w > 0 && dev->readback_ring[i].h > 0) {
            if (best < 0 || dev->readback_ring[i].tag < dev->readback_ring[best].tag)
                best = i;
        }
    }
    if (best < 0) return false;
    int w = dev->readback_ring[best].w, h = dev->readback_ring[best].h;
    size_t need = (size_t)w * (size_t)h * 4u;
    dev->readback_ring[best].pending = false;
    if (!out || out_bytes < need) return false;
    readback_swizzle(dev, dev->readback_ring[best].staging->mapped, out, w, h);
    if (out_w)   *out_w   = w;
    if (out_h)   *out_h   = h;
    if (out_tag) *out_tag = dev->readback_ring[best].tag;
    return true;
}

bool rhi_read_render_target(rhi_device *dev, rhi_texture *rt, void *out,
                            size_t out_bytes, int *out_w, int *out_h)
{
    if (!rt || !rt->is_rt) return false;
    int w = (int)rt->width, h = (int)rt->height;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    size_t need = (size_t)w * (size_t)h * 4u;
    if (!out || out_bytes < need || w <= 0 || h <= 0) return false;

    /* The RT is left in SHADER_READ_ONLY by rhi_end_render_target. Wait for the
     * frame that rendered it, transition to TRANSFER_SRC, copy to a host buffer,
     * then restore SHADER_READ_ONLY so it stays sampleable / safe to destroy. */
    vkDeviceWaitIdle(dev->device);
    rhi_buffer *rb = create_host_buffer(dev, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        /*prefer_cached=*/true);
    if (!rb) return false;

    VkCommandBuffer cb = one_shot_begin(dev);
    image_barrier(cb, rt->image,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = {
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .imageExtent = { (uint32_t)w, (uint32_t)h, 1 },
    };
    vkCmdCopyImageToBuffer(cb, rt->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           rb->buffer, 1, &region);
    image_barrier(cb, rt->image,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    one_shot_end(dev, cb);
    rt->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    /* Same B8G8R8A8 -> R,G,B,A swizzle + opaque alpha as rhi_read_offscreen. */
    const uint8_t *src = (const uint8_t *)rb->mapped;
    uint8_t *dst = (uint8_t *)out;
    int swap_rb = (dev->sc_format == VK_FORMAT_B8G8R8A8_UNORM ||
                   dev->sc_format == VK_FORMAT_B8G8R8A8_SRGB);
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        uint8_t c0 = src[i*4+0], c1 = src[i*4+1], c2 = src[i*4+2];
        dst[i*4+0] = swap_rb ? c2 : c0;
        dst[i*4+1] = c1;
        dst[i*4+2] = swap_rb ? c0 : c2;
        dst[i*4+3] = 0xFF;
    }
    rhi_destroy_buffer(dev, rb);
    return true;
}

void rhi_destroy_texture(rhi_device *dev, rhi_texture *tex)
{
    if (!tex) return;
    if (tex->set)     vkFreeDescriptorSets(dev->device, dev->desc_pool, 1, &tex->set);
    if (tex->staging) rhi_destroy_buffer(dev, tex->staging);
    if (tex->view)    vkDestroyImageView(dev->device, tex->view, NULL);
    if (tex->image)   vkDestroyImage(dev->device, tex->image, NULL);
    if (tex->mem)     vkFreeMemory(dev->device, tex->mem, NULL);
    /* Render-target extras. */
    if (tex->rt_fb)         vkDestroyFramebuffer(dev->device, tex->rt_fb, NULL);
    if (tex->rt_depth_view) vkDestroyImageView(dev->device, tex->rt_depth_view, NULL);
    if (tex->rt_depth_image) vkDestroyImage(dev->device, tex->rt_depth_image, NULL);
    if (tex->rt_depth_mem)  vkFreeMemory(dev->device, tex->rt_depth_mem, NULL);
    free(tex);
}

/* ------------------------------------------------------------------ */
/* Pipelines                                                          */
/* ------------------------------------------------------------------ */

/* Build a shader module from an in-memory SPIR-V blob. vkCreateShaderModule
 * requires pCode 4-byte aligned, which an embedded `unsigned char[]` is not
 * guaranteed to be, so we always feed a fresh malloc copy (malloc is suitably
 * aligned). The blobs are tiny and this runs once at startup. */
static VkShaderModule create_shader_module(rhi_device *dev, const void *spv,
                                           size_t n, const char *name)
{
    if (n == 0 || (n % 4) != 0) {
        RHI_LOG("bad SPIR-V size %zu: %s\n", n, name); return VK_NULL_HANDLE;
    }
    uint32_t *code = malloc(n);
    if (!code) { RHI_LOG("oom for shader: %s\n", name); return VK_NULL_HANDLE; }
    memcpy(code, spv, n);
    VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = n,
        .pCode = code,
    };
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev->device, &ci, NULL, &m) != VK_SUCCESS) {
        RHI_LOG("vkCreateShaderModule failed: %s\n", name); m = VK_NULL_HANDLE;
    }
    free(code);
    return m;
}

/* Look up an embedded SPIR-V blob by its .spv filename (the same string the
 * pipeline descs use). Returns NULL if not found. */
static const void *find_embedded_shader(const char *filename, size_t *out_size)
{
    for (unsigned i = 0; i < g_rhi_embedded_shader_count; ++i) {
        if (strcmp(g_rhi_embedded_shaders[i].name, filename) == 0) {
            *out_size = g_rhi_embedded_shaders[i].size;
            return g_rhi_embedded_shaders[i].data;
        }
    }
    return NULL;
}

static VkShaderModule load_shader_module(rhi_device *dev, const char *filename)
{
    /* Dev/modding override: OPENPETE_SHADER_DIR=<dir> loads loose .spv from
     * disk (e.g. the build's core/rhi/shaders) so a shader edit applies on the
     * next launch without relinking. Unset in shipping builds, where the
     * embedded SPIR-V baked in at build time is used and the binary needs no
     * shaders/ dir. A missing/unreadable file falls through to embedded. */
    const char *dir = getenv("OPENPETE_SHADER_DIR");
    if (dir && dir[0]) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, filename);
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0) {
                void *buf = malloc((size_t)n);
                if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
                    fclose(f);
                    VkShaderModule m = create_shader_module(dev, buf, (size_t)n, path);
                    free(buf);
                    return m;
                }
                free(buf);
            }
            fclose(f);
            RHI_LOG("OPENPETE_SHADER_DIR: failed to read %s; using embedded\n", path);
        } else {
            RHI_LOG("OPENPETE_SHADER_DIR: %s not found; using embedded\n", path);
        }
    }

    /* Default/shipping path: the SPIR-V embedded in the binary. */
    size_t n = 0;
    const void *spv = find_embedded_shader(filename, &n);
    if (!spv) { RHI_LOG("no embedded shader: %s\n", filename); return VK_NULL_HANDLE; }
    return create_shader_module(dev, spv, n, filename);
}

rhi_pipeline *rhi_create_pipeline(rhi_device *dev, const rhi_pipeline_desc *desc)
{
    VkShaderModule vs = load_shader_module(dev, desc->vert_spv);
    VkShaderModule fs = load_shader_module(dev, desc->frag_spv);
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        if (vs) vkDestroyShaderModule(dev->device, vs, NULL);
        if (fs) vkDestroyShaderModule(dev->device, fs, NULL);
        return NULL;
    }

    rhi_pipeline *p = calloc(1, sizeof(*p));
    if (!p) {
        vkDestroyShaderModule(dev->device, vs, NULL);
        vkDestroyShaderModule(dev->device, fs, NULL);
        return NULL;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,   .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
    };
    VkVertexInputBindingDescription bind = {
        .binding = 0, .stride = sizeof(rhi_vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    /* The vertex STRUCT keeps all fields (stable buffer layout), but the
     * pipeline only declares the attributes the current shaders consume.
     * `tex` (location 3, offset in rhi_vertex) returns in 1b.2 when the
     * PSX texture shader reads it — declaring it now just warns
     * "attribute not consumed". */
    VkVertexInputAttributeDescription attrs[3] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(rhi_vertex, x) },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,    .offset = offsetof(rhi_vertex, u) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UNORM,   .offset = offsetof(rhi_vertex, rgba) },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bind,
        .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = attrs,
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState cba = {
        .blendEnable = (desc->blend != RHI_BLEND_NONE) ? VK_TRUE : VK_FALSE,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .colorBlendOp = VK_BLEND_OP_ADD, .alphaBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
    };
    if (desc->blend == RHI_BLEND_ALPHA) {
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    } else if (desc->blend == RHI_BLEND_ADD) {
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    } else if (desc->blend == RHI_BLEND_SUBTRACT) {
        /* PSX ABR mode 2: dst = B - F  (reverse-subtract, src ONE, dst ONE). */
        cba.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.alphaBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
    } else {
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    }
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba,
    };
    /* Depth: the offscreen target always carries a depth attachment (3a.3);
     * a pipeline opts into testing/writing it via desc->depth_test. The 2D
     * panels (depth_test=false) ignore it and never write it. LESS_OR_EQUAL so
     * equal-depth later draws (e.g. coincident prims) still paint. */
    VkPipelineDepthStencilStateCreateInfo dss = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable  = desc->depth_test ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = (desc->depth_test && !desc->depth_write_off) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
    };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn,
    };
    /* Optional VRAM sampler set (0) + a fragment-stage push-constant block
     * (the per-draw PSX material: tpage/clut/bpp). The 1b.1 quad pipeline
     * uses neither and keeps an empty layout. */
    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0, .size = desc->push_const_size,
    };
    /* set 0 (uses_vram) + optional set 1 (uses_vram2, e.g. the diff pass's
     * reference sampler) — both reuse the single-binding vram set layout. */
    VkDescriptorSetLayout set_layouts[2] = { dev->vram_set_layout, dev->vram_set_layout };
    uint32_t set_count = desc->uses_vram ? (desc->uses_vram2 ? 2u : 1u) : 0u;
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = set_count,
        .pSetLayouts = set_count ? set_layouts : NULL,
        .pushConstantRangeCount = desc->push_const_size ? 1u : 0u,
        .pPushConstantRanges = desc->push_const_size ? &pcr : NULL,
    };
    if (vkCreatePipelineLayout(dev->device, &plci, NULL, &p->layout) != VK_SUCCESS) {
        RHI_LOG("vkCreatePipelineLayout failed\n");
        vkDestroyShaderModule(dev->device, vs, NULL);
        vkDestroyShaderModule(dev->device, fs, NULL);
        free(p); return NULL;
    }
    VkGraphicsPipelineCreateInfo gp = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pDepthStencilState = &dss,
        .pColorBlendState = &cb,
        .pDynamicState = &ds,
        .layout = p->layout,
        .renderPass = dev->off_rp,
        .subpass = 0,
    };
    VkResult r = vkCreateGraphicsPipelines(dev->device, VK_NULL_HANDLE, 1, &gp, NULL, &p->pipeline);
    vkDestroyShaderModule(dev->device, vs, NULL);
    vkDestroyShaderModule(dev->device, fs, NULL);
    if (r != VK_SUCCESS) {
        RHI_LOG("vkCreateGraphicsPipelines failed: %d\n", (int)r);
        vkDestroyPipelineLayout(dev->device, p->layout, NULL);
        free(p); return NULL;
    }
    return p;
}

void rhi_destroy_pipeline(rhi_device *dev, rhi_pipeline *pipe)
{
    if (!pipe) return;
    if (pipe->pipeline) vkDestroyPipeline(dev->device, pipe->pipeline, NULL);
    if (pipe->layout)   vkDestroyPipelineLayout(dev->device, pipe->layout, NULL);
    free(pipe);
}

void rhi_bind_pipeline(rhi_device *dev, rhi_pipeline *pipe)
{
    if (!dev->frame_active) return;
    dev->cur_layout = pipe->layout; /* for bind_vram / push_constants */
    vkCmdBindPipeline(dev->cmd[dev->frame], VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->pipeline);
}

void rhi_bind_vertex_buffer(rhi_device *dev, rhi_buffer *buf)
{
    if (!dev->frame_active) return;
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(dev->cmd[dev->frame], 0, 1, &buf->buffer, &off);
}

void rhi_bind_vram(rhi_device *dev, rhi_texture *tex)
{
    if (!dev->frame_active) return;
    vkCmdBindDescriptorSets(dev->cmd[dev->frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                            dev->cur_layout, 0, 1, &tex->set, 0, NULL);
}

void rhi_bind_vram2(rhi_device *dev, rhi_texture *tex)
{
    if (!dev->frame_active) return;
    vkCmdBindDescriptorSets(dev->cmd[dev->frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                            dev->cur_layout, 1, 1, &tex->set, 0, NULL);
}

void rhi_push_constants(rhi_device *dev, const void *data, uint32_t size)
{
    if (!dev->frame_active) return;
    vkCmdPushConstants(dev->cmd[dev->frame], dev->cur_layout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, size, data);
}

void rhi_draw(rhi_device *dev, uint32_t vertex_count, uint32_t first_vertex)
{
    if (!dev->frame_active) return;
    vkCmdDraw(dev->cmd[dev->frame], vertex_count, 1, first_vertex, 0);
}

void rhi_set_scissor(rhi_device *dev, int x, int y, int w, int h)
{
    if (!dev->frame_active) return;
    /* Callers pass swapchain/window pixels. When the bound pass targets a
     * larger render area (the SSAA supersampled scene RT, whose extent is an
     * integer multiple of the swapchain), scale the rect by that factor so the
     * panel scissor still confines the N×-resolution scene. (No-op when the
     * current pass matches the swapchain extent, i.e. the 1× offscreen.) */
    if (dev->sc_extent.width &&
        dev->cur_extent.width  > dev->sc_extent.width &&
        dev->cur_extent.width  % dev->sc_extent.width == 0) {
        int eff = (int)(dev->cur_extent.width / dev->sc_extent.width);
        x *= eff; y *= eff; w *= eff; h *= eff;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (x + w > (int)dev->cur_extent.width)  w = (int)dev->cur_extent.width  - x;
    if (y + h > (int)dev->cur_extent.height) h = (int)dev->cur_extent.height - y;
    VkRect2D s = { .offset = { x, y }, .extent = { (uint32_t)(w > 0 ? w : 0),
                                                   (uint32_t)(h > 0 ? h : 0) } };
    vkCmdSetScissor(dev->cmd[dev->frame], 0, 1, &s);
}

void rhi_reset_scissor(rhi_device *dev)
{
    if (!dev->frame_active) return;
    VkRect2D s = { .offset = { 0, 0 }, .extent = dev->cur_extent };
    vkCmdSetScissor(dev->cmd[dev->frame], 0, 1, &s);
}
