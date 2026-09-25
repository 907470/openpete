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

#include "rhi_gpu_audit.h" /* must follow volk.h: redirects the vk* globals */
#include "rhi_memtype.h"

/* Acquire budget, and the back-off after one expires.
 *
 * The budget has TWO bounds and the original only had the upper one. It must
 * stay well under the pacer's slip threshold (4 sim periods, ~67 ms), or every
 * expiry becomes a reported sim slip. It must ALSO exceed one display refresh
 * period — because under FIFO, presenting at any rate below the refresh means
 * the next image legitimately becomes available at the next repaint, and a
 * budget shorter than that expires on essentially every present.
 *
 * The old fixed 8 ms satisfied only the upper bound, and 8 ms is shorter than
 * a 120 Hz period (8.33 ms). Measured on a 120 Hz laptop at --render-fps 60:
 * 1855 budget expiries, each opening the back-off below, which then suppressed
 * the next present — 3710 refusals and **2 frames displayed in 150 seconds**,
 * while the pacing log happily reported 3698 presents. Raising the budget to
 * 24 ms on the same machine and scene: 0 refusals, 3698 frames, and the phase
 * error IMPROVED too (94.91 % -> 99.59 % within 250 us, max 36.4 -> 6.0 ms).
 * There was no trade-off to make; the short budget was pure loss, because the
 * back-off cost the sim more time than waiting for the image ever did.
 *
 * So it is derived from the refresh at swapchain creation
 * (rhi_acquire_budget_ns) rather than fixed. This constant is the floor used
 * when the refresh is unknown, and it must remain >= one period of the slowest
 * display we expect to meet.
 *
 * The back-off is what stops a sustained stall (a live resize holds images
 * for as long as the user drags) from costing the budget on EVERY iteration
 * and throttling the sim to a crawl. After one expiry, presents are skipped
 * outright for a short window — no blocking at all — so the sim runs at full
 * speed and presentation resumes on its own once images flow again. */
/* Used only when the refresh cannot be read: assume the slowest display we
 * expect to meet (60 Hz) rather than a fast one, because guessing too SHORT
 * is the failure that hides itself — it expires, backs off, and reports
 * presents that never reached the screen. */
#ifndef RHI_ACQUIRE_TIMEOUT_NS
#define RHI_ACQUIRE_TIMEOUT_NS  (25ull * 1000ull * 1000ull)   /* ~1.5 x 60 Hz */
#endif
/* Floor for the derived budget. A refresh so high that 1.5 periods is under
 * this is not the case worth optimising; the value only has to exceed the
 * driver's own acquire latency. */
#ifndef RHI_ACQUIRE_BUDGET_MIN_NS
#define RHI_ACQUIRE_BUDGET_MIN_NS (4ull * 1000ull * 1000ull)   /* 4 ms */
#endif
/* Must match the renderer's own resize settle window closely enough that the
 * swapchain and the render targets rebuild in the same gesture. */
#ifndef RHI_RESIZE_SETTLE_NS
#define RHI_RESIZE_SETTLE_NS    (150ull * 1000ull * 1000ull)  /* 150 ms */
#endif
/* Backstop only: by this point the slot poll has already passed, so this
 * fence is normally signalled. Bounding it keeps a driver hiccup from parking
 * the sim thread indefinitely, the same reasoning as the acquire timeout. */
/* Bound for the waits a dedicated present thread makes. Long enough for a
 * swapchain rebuild plus the slowest display's refresh (a 60 Hz panel under
 * FIFO with the queue full is ~50 ms of legitimate waiting); short enough
 * that a dead compositor returns control. */
#ifndef RHI_STAGE_RING
#define RHI_STAGE_RING 3
#endif
#ifndef RHI_BLOCKING_FENCE_TIMEOUT_NS
#define RHI_BLOCKING_FENCE_TIMEOUT_NS (500ull * 1000ull * 1000ull)
#endif
#ifndef RHI_IMAGE_FENCE_TIMEOUT_NS
#define RHI_IMAGE_FENCE_TIMEOUT_NS (32ull * 1000ull * 1000ull)   /* 32 ms */
#endif
/* Back-off after an expiry. NOT 32 ms: that sat 1.4 ms from the canonical
 * present period (33.4 ms), so one expiry suppressed exactly every other
 * present in a stable alternating pattern rather than decaying. A back-off
 * must not be commensurate with the cadence it interrupts. One VBL is short
 * enough to recover within a tick and long enough to break a stall loop. */
/* Hard cap on the derived budget: an expiry must never be able to become a
 * reported sim slip (the pacer clamps at 4 sim periods, ~67 ms). */
#ifndef RHI_ACQUIRE_BUDGET_MAX_NS
#define RHI_ACQUIRE_BUDGET_MAX_NS (40ull * 1000ull * 1000ull)  /* 40 ms */
#endif
/* Shortest wait that counts as having OBSERVED a flip. Below this the frame
 * was already on screen and the return time is when we asked, not when the
 * display changed. */
#ifndef RHI_FLIP_OBSERVED_NS
#define RHI_FLIP_OBSERVED_NS    (20ull * 1000ull)              /* 20 us */
#endif
#ifndef RHI_ACQUIRE_BACKOFF_NS
#define RHI_ACQUIRE_BACKOFF_NS  (16ull * 1000ull * 1000ull)   /* ~1 VBL */
#endif

/* Swapchain pipeline depth. Overridable from the build so the depth/refresh
 * trade can be swept without editing the source; every per-frame ring below
 * is sized from it. Note the readback and grid rings lag by exactly this many
 * presents, so a capture taken at depth N lands N presents behind. */
#ifndef RHI_FRAMES_IN_FLIGHT
#define RHI_FRAMES_IN_FLIGHT 2
#endif
#define RHI_MAX_SC_IMAGES    8

#define RHI_LOG(...)  fprintf(stderr, "[rhi/vk] " __VA_ARGS__)

/* Heap-balloon block size, the per-spec block cap (32 GiB of blocks) and the
 * block-list length. The cap is per spec so a spec filling a second heap
 * cannot be starved by the blocks an earlier spec already holds. */
#define RHI_BALLOON_BLOCK_MIB  64u
#define RHI_BALLOON_SPEC_CAP   512
#define RHI_BALLOON_MAX_BLOCKS 1024

/* Handler invoked on an unrecoverable VK_ERROR_DEVICE_LOST (registered by the
 * core layer, which owns the shutdown flag) instead of spinning failed frames
 * forever. A callback keeps openpete_rhi free of core-runtime symbols — the
 * link goes core -> rhi, not back. */
/* The runtime half of the display-seam fence (the build-time half is
 * scripts/fences/seam_guard.py). The display's frame/present/swapchain
 * state is externally synchronised and owned by exactly one thread at a
 * time; ownership moves only through rhi_display_claim, called by the
 * owner-to-be at each handoff. Every entry point below verifies the
 * calling thread. A violation logs (first few in full, then counted) so a
 * shipped build never dies for it; OPENPETE_SEAM_GUARD_FATAL=1 aborts at
 * the first, which is what the stress harnesses run with — a fault caught
 * here names the interleaving that a driver would otherwise turn into a
 * silent process kill minutes later. */
static SDL_ThreadID s_display_owner;
static unsigned     s_seam_violations;
void rhi_display_claim(rhi_device *dev)
{
    (void)dev;
    s_display_owner = SDL_GetCurrentThreadID();
}
static void seam_check(const char *where)
{
    if (!s_display_owner) return;                 /* pre-claim boot window */
    const SDL_ThreadID me = SDL_GetCurrentThreadID();
    if (me == s_display_owner) return;
    static int fatal = -1;
    if (fatal < 0) { const char *e = getenv("OPENPETE_SEAM_GUARD_FATAL"); fatal = (e && *e && *e != '0'); }
    if (s_seam_violations < 8)
        RHI_LOG("SEAM: %s on thread %" SDL_PRIu64 " but the display is owned by %" SDL_PRIu64 "\n",
                where, (Uint64)me, (Uint64)s_display_owner);
    s_seam_violations++;
    if (fatal) abort();
}
unsigned rhi_seam_violations(void) { return s_seam_violations; }

static void (*s_device_lost_cb)(void) = NULL;
void rhi_set_device_lost_handler(void (*fn)(void)) { s_device_lost_cb = fn; }
static const char *s_last_gpu_zone;   /* set by rhi_gpu_zone_begin */
static void rhi_signal_device_lost(const char *where) {
    RHI_LOG("FATAL: device lost on %s — requesting shutdown\n", where);
    if (s_last_gpu_zone)
        RHI_LOG("device-lost suspect: last GPU zone begun = \"%s\" "
                "(a runaway mod shader TDRs here)\n", s_last_gpu_zone);
    else
        RHI_LOG("re-run with OPENPETE_GPU_TIMERS=1 to attribute the hang "
                "to a named pass/mod\n");
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

/* Two-sampler sets one frame slot may take (rhi_bind_vram_depth_pair). */
#define RHI_PAIR_SETS_MAX 32

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
    char                     driver_version[64];
    bool                     device_software; /* picked device is a CPU rasterizer (llvmpipe) */
    bool                     headless;   /* surfaceless: no swapchain / present */
    int                      vsync_pref; /* --vsync: 1 = FIFO (vsync ON), 0 = vsync
                                          * OFF, -1 = unset (env / default). Read by
                                          * choose_present_mode on (re)create. */
    uint64_t                 fence_sheds; /* presents skipped rather than
                                           * blocking the caller on the
                                           * display's cadence */
    VkPresentModeKHR         present_mode_granted; /* what the swapchain was
                                          * actually built with -- the requested
                                          * mode is not evidence of anything. */

    /* Heap balloon (OPENPETE_RHI_BALLOON): device memory held, unmapped and
     * unbound, so the driver refuses later allocations in that heap. Freed
     * before the device is destroyed. */
    VkDeviceMemory           balloon_blocks[RHI_BALLOON_MAX_BLOCKS];
    int                      balloon_count;

    /* Swapchain */
    VkSwapchainKHR swapchain;
    VkFormat       sc_format;
    VkFormat       depth_format;  /* combined depth+stencil (W2.6): D32_SFLOAT_S8
                                   * preferred, D24_UNORM_S8 fallback — picked at
                                   * device selection (pick_depth_format). */
    /* The depth convention per rhi_depth_class, set with the format
     * (depth_convention_init): the compare op a pipeline of the class
     * tests with, and the value a pass of the class clears depth to. */
    VkCompareOp    depth_cmp[RHI_DEPTH_CLASSES];
    float          depth_clear[RHI_DEPTH_CLASSES];
    VkBool32       depth_rev[RHI_DEPTH_CLASSES]; /* spec constant 0 (DEPTH_REVERSED) */
    float          cur_depth_clear;  /* clear value of the current pass (rhi_clear_depth) */
    unsigned       pipes_by_cmp[8];  /* pipelines compiled per VkCompareOp (engagement) */
    bool           has_depth_bias_clamp; /* depthBiasClamp feature enabled */
    bool           has_shader_float64;   /* shaderFloat64 feature enabled  */
    bool           has_fill_non_solid;   /* fillModeNonSolid feature enabled */
    VkExtent2D     sc_extent;
    uint32_t       sc_count;
    VkImage        sc_images[RHI_MAX_SC_IMAGES];

    /* Offscreen color target (== swapchain extent). SSAA supersampling lives in
     * the renderer as a separate render target that resolves into this. */
    VkRenderPass   off_rp;
    VkRenderPass   off_load_rp;   /* off_rp but color LOADs (SCENE postfx re-begin, S5) */
    /* Render-to-texture pass: same attachment formats as off_rp (so off_rp-built
     * pipelines are compatible) but the color attachment ends in
     * SHADER_READ_ONLY_OPTIMAL so the result can be sampled the same frame. */
    VkRenderPass   rt_rp;
    /* Depth-sampled RT pass (the shadow-decal depth pre-pass): like rt_rp but the
     * DEPTH attachment is STORED and ends in SHADER_READ_ONLY_OPTIMAL too, so a
     * later pass in the same frame can sample the per-pixel depth. (rt_rp's depth
     * is storeOp=DONT_CARE — sampling it after the pass would read undefined
     * memory per spec, even where drivers happen to preserve it.) Attachment
     * formats match rt_rp/off_rp, so all existing pipelines are compatible. */
    VkRenderPass   rt_ds_rp;
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

    /* Staged presents (rhi_end_frame_to_stage / rhi_present_stage): a ring
     * of swapchain-sized images a rendered frame is blitted into instead of
     * the swapchain, so the render can run ahead of the instant the frame is
     * meant to appear, and the present step is an acquire and a blit. All
     * synchronisation between the two is queue submission order plus image
     * barriers -- no semaphore crosses them, so a staged frame that is never
     * presented (superseded, or the display was taken back) leaves nothing
     * pending. Recreated with the swapchain; `stage_gen` tells a holder of a
     * stage index that its image is gone. */
    VkImage         stage_image[RHI_STAGE_RING];
    VkDeviceMemory  stage_mem[RHI_STAGE_RING];
    VkImageLayout   stage_layout[RHI_STAGE_RING];
    uint32_t        stage_gen;
    VkCommandBuffer pcmd[RHI_STAGE_RING];       /* the present step's own ring */
    VkFence         pfence[RHI_STAGE_RING];
    VkSemaphore     pimg_avail[RHI_STAGE_RING];
    uint32_t        pslot;
    bool            pacquired;      /* rhi_present_stage_acquire holds an image for the next submit */
    uint32_t        pacquired_gen;
    uint32_t        pacquired_index; /* WHICH image that acquire returned: image_index is clobbered
                                      * by any begin_frame in between (the inline path, after the
                                      * present thread hands the display back), and presenting an
                                      * image the held semaphore was not armed for faults the driver. */

    /* All live pipelines on this device (doubly linked via rhi_pipeline
     * prev/next): the sweep list for rhi_rebuild_pipelines. */
    struct rhi_pipeline *pipes_head;

    uint32_t frame;        /* 0..RHI_FRAMES_IN_FLIGHT-1 */
    uint32_t image_index;  /* acquired swapchain image this frame */
    bool     needs_recreate;
    /* Presents dropped because no swapchain image arrived in time — a live
     * resize is the expected cause. Counted so a silent frame drop is
     * visible rather than mysterious. */
    unsigned acquire_timeouts;
    /* Presents skipped outright because a previous acquire expired and the
     * back-off window is still open. Counted separately from the timeouts
     * themselves: one expiry can suppress several presents, and a caller that
     * cannot tell them apart cannot tell a display running at half rate from
     * one running at full rate. */
    uint64_t acquire_sheds;
    /* Acquire budget in ns, derived from the display refresh at swapchain
     * creation. See RHI_ACQUIRE_TIMEOUT_NS for why a fixed one was wrong. */
    Uint64   acquire_budget_ns;
    /* The presenting thread has nothing else to do: block on the slot fence
     * and the acquire instead of shedding. Set by a dedicated present thread;
     * clear while the thread that runs the sim presents. */
    bool     present_blocking;
    float    refresh_hz;          /* of the display the swapchain was built for (0 = unknown) */
    /* While set, presents are skipped without blocking (see the acquire
     * budget above). Cleared once the window passes. */
    Uint64   acquire_backoff_until_ns;
    /* Same idea for swapchain rebuilds, which are far more expensive. */
    Uint64   recreate_backoff_until_ns;
    /* Window size as last observed while waiting for a resize to settle. */
    int      last_seen_w, last_seen_h;
    Uint64   size_stable_since_ns;
    /* Display timing (VK_KHR_present_id + VK_KHR_present_wait), when the
     * driver offers both. Each present carries an id; waiting on an id blocks
     * until that present has been DISPLAYED, which is the only number here
     * that is not a submit time. */
    bool     has_present_timing;
    uint64_t present_id_next;      /* id the NEXT present will carry (from 1) */
    uint64_t present_id_submitted; /* id of the last present actually queued  */
    uint64_t present_id_seen;      /* newest id confirmed DISPLAYED           */
    /* Set when the caller knows the size change was DISCRETE — one the app
     * caused, arriving in a single step. The settle window exists to absorb a
     * drag's per-pixel walk; applying it to a fullscreen toggle just freezes
     * the display for the length of the window. */
    bool     resize_discrete;
    bool     frame_active;  /* between begin_frame and end_frame */
    uint32_t draw_calls;    /* rhi_draw invocations since begin_frame (perf census) */
    VkExtent2D cur_extent;  /* render area of the CURRENT pass (offscreen sc_extent
                            * or the bound render target) — for rhi_clear_depth */

    float clear[4];

    /* VRAM-sampler infrastructure (1b.2): one shared sampler + set layout +
     * pool; each rhi_texture owns a descriptor set allocated from the pool. */
    VkSampler             vram_sampler;
    VkSampler             linear_sampler;   /* LINEAR twin for HD replacement art */
    VkDescriptorSetLayout vram_set_layout;
    VkDescriptorPool      desc_pool;
    VkPipelineLayout      cur_layout;  /* layout of the bound pipeline */
    VkShaderStageFlags    cur_pc_stages; /* its push-constant stage flags */

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

    /* Second async ring for rhi_cycle_rt_readback (the occlusion-grid RT — a
     * few-KB color image read back every present). Same fence discipline as
     * readback_ring; single client (one RT per device), so no per-RT rings. */
    struct {
        rhi_buffer *staging;
        bool        pending;
        int         w, h;
    } grid_ring[RHI_FRAMES_IN_FLIGHT];

    /* GPU per-pass timestamp zones (OPENPETE_GPU_TIMERS=1): one timestamp
     * query pool per frame in flight, 2 queries per zone. Slot i's results are
     * harvested in rhi_begin_frame right after slot i's fence wait — the same
     * discipline as readback_ring, so retrieval never blocks (results arrive
     * RHI_FRAMES_IN_FLIGHT presents late; fine for a diagnostic). Both zone
     * timestamps are BOTTOM_OF_PIPE: elapsed = previous work drained → this
     * zone's work drained, which attributes correctly on a serial queue and
     * degrades to an approximation where the GPU overlaps passes. */
    /* S2 mod-shader extensions: 2-binding dynamic-UBO set layout (postfx
     * set 3), per-frame-slot descriptor sets for sampling off_image (the
     * COMPOSITE postfx input — lazily (re)written when off_gen moves, i.e.
     * after every offscreen recreate), and the UBO offset alignment. */
    VkDescriptorSetLayout ubo_set_layout;
    VkDescriptorSetLayout ubo1_set_layout;            /* one dynamic binding    */
    VkDescriptorSet off_sampled_set[RHI_FRAMES_IN_FLIGHT];
    uint32_t        off_gen;                          /* bumped per create_offscreen */
    uint32_t        off_sampled_gen[RHI_FRAMES_IN_FLIGHT];
    VkDeviceSize    ubo_align;                        /* minUniformBufferOffsetAlignment */
    uint32_t        max_desc_sets;                    /* maxBoundDescriptorSets          */
    /* Two-sampler set layout (binding 0 = a colour texture, binding 1 = a
     * sampled depth image) for pipelines with set1_vram_depth. Its sets are
     * written per bind: each bind in a frame takes the next set of the frame
     * slot's list, grown on demand, and the list's cursor rewinds when the
     * slot's fence has retired (begin_frame_impl), so a set is never
     * rewritten while a recorded draw still reads it. */
    VkDescriptorSetLayout pair_set_layout;
    VkDescriptorSet pair_sets[RHI_FRAMES_IN_FLIGHT][RHI_PAIR_SETS_MAX];
    uint32_t        pair_alloc[RHI_FRAMES_IN_FLIGHT]; /* sets allocated per slot */
    uint32_t        pair_used[RHI_FRAMES_IN_FLIGHT];  /* sets taken this frame   */

    bool        gpu_timers;                 /* env set AND queue has timestamps */
    float       gpu_ts_period;              /* ns per timestamp tick */
    VkQueryPool gpu_qp[RHI_FRAMES_IN_FLIGHT];
    const char *gpu_zone_name[RHI_FRAMES_IN_FLIGHT][RHI_GPU_ZONES_MAX];
    uint8_t     gpu_zone_depth[RHI_FRAMES_IN_FLIGHT][RHI_GPU_ZONES_MAX];
    int         gpu_zone_n[RHI_FRAMES_IN_FLIGHT];   /* zones begun this frame */
    int         gpu_zone_stack[RHI_GPU_ZONES_MAX];  /* open-zone index stack   */
    int         gpu_zone_sp;                        /* nesting depth           */
    uint64_t    gpu_zone_token[RHI_FRAMES_IN_FLIGHT];   /* caller tag @ submit  */
    rhi_gpu_zone_result gpu_results[RHI_GPU_ZONES_MAX];
    int         gpu_results_n;
    uint64_t    gpu_results_token;                  /* tag of harvested slot   */
    /* Storage a grown ring vertex buffer replaced (rhi_vertex_buffer_reserve),
     * kept until no submitted frame can still read it. */
    struct rhi_retired_buf *retired;
};

/* One replaced buffer allocation and the frame serial current when it was
 * replaced: frames up to that serial may have recorded binds of it. */
struct rhi_retired_buf {
    struct rhi_retired_buf *next;
    VkBuffer       buffer;
    VkDeviceMemory memory;
    uint64_t       serial;
};
static void retired_reclaim(rhi_device *dev, bool all);

struct rhi_buffer {
    VkBuffer       buffer;
    VkDeviceMemory memory;
    void          *mapped;
    VkDeviceSize   size;
    /* Dynamic per-frame ring (rhi_create_vertex_buffer_dynamic): the buffer
     * holds RHI_FRAMES_IN_FLIGHT slots of `ring_stride` bytes; update writes
     * and bind reads the CURRENT frame's slot, so a per-present rewrite can
     * never race a previous in-flight frame's GPU read (the frame's fence was
     * waited in rhi_begin_frame before its slot is reused). 0 = single
     * (static) buffer. */
    VkDeviceSize   ring_stride;
    /* Generations within one frame slot (rhi_create_vertex_buffer_dynamic_gens):
     * a write that follows a bind in the same frame moves to the next
     * generation, so the draw already recorded against the previous data
     * keeps it; the GPU reads host-visible memory at execution, not at
     * record. Physical slot = frame * gens + gen. gens <= 1: one generation,
     * the write overwrites in place. */
    uint32_t       gens, gen_cur;
    uint64_t       gen_serial;   /* frame serial gen_cur/gen_bound belong to */
    bool           gen_bound;    /* current generation referenced by a draw */
    rhi_device    *dev;        /* for frame indexing in rhi_update_buffer */
};

/* Same-frame rewrite accounting (engagement evidence for the generation
 * mechanism): bumps = writes that moved to a fresh generation; overflows =
 * writes that had no generation left and overwrote bound data in place. */
static uint64_t s_ring_gen_bumps, s_ring_gen_overflows, s_dbg_gw, s_dbg_gb;
static uint64_t s_frame_serial;
static bool ring_gen_dbg(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("OPENPETE_RING_GEN_DBG"); v = (e && *e && *e != '0'); }
    return v;
}
/* Slot index for the next write of a ring buffer inside the active frame. */
static uint32_t ring_write_slot(rhi_buffer *buf)
{
    rhi_device *dev = buf->dev;
    if (buf->gens <= 1) return (uint32_t)dev->frame;
    s_dbg_gw++;
    if (buf->gen_serial != s_frame_serial) {
        buf->gen_serial = s_frame_serial; buf->gen_cur = 0; buf->gen_bound = false;
    } else if (buf->gen_bound) {
        if (buf->gen_cur + 1 < buf->gens) { buf->gen_cur++; s_ring_gen_bumps++; }
        else {
            s_ring_gen_overflows++;
            static bool once; if (!once) { once = true;
                RHI_LOG("ring buffer: %u generations exhausted in frame %llu; overwriting bound data\n",
                        buf->gens, (unsigned long long)s_frame_serial); }
        }
        buf->gen_bound = false;
    }
    return (uint32_t)dev->frame * buf->gens + buf->gen_cur;
}
/* Slot index a bind inside the active frame references. */
static uint32_t ring_bind_slot(rhi_buffer *buf)
{
    rhi_device *dev = buf->dev;
    if (!buf || buf->gens <= 1) return (uint32_t)dev->frame;
    s_dbg_gb++;
    if (buf->gen_serial != s_frame_serial) {
        buf->gen_serial = s_frame_serial; buf->gen_cur = 0;
    }
    buf->gen_bound = true;
    return (uint32_t)dev->frame * buf->gens + buf->gen_cur;
}
uint64_t rhi_ring_gen_bumps(void) { return s_ring_gen_bumps; }
uint64_t rhi_ring_gen_overflows(void) { return s_ring_gen_overflows; }

struct rhi_pipeline {
    VkPipeline       pipeline;
    VkPipelineLayout layout;
    /* Rebuild sources (rhi_rebuild_pipelines): the desc plus owned copies of
     * the shader identities — embedded/loose filenames, or in-memory SPIR-V
     * for mod shaders — so any pipeline can be re-baked with new vertex
     * spec-constant values without its owner's involvement. desc.vert_spv /
     * desc.frag_spv point at the owned name copies (NULL when words given). */
    rhi_pipeline_desc desc;
    char            *vert_name, *frag_name;   /* owned filename copies      */
    uint32_t        *vert_words, *frag_words; /* owned SPIR-V word copies   */
    size_t           vert_n, frag_n;          /* word counts                */
    struct rhi_pipeline *prev, *next;         /* device-wide rebuild registry */
    VkShaderStageFlags pc_stages; /* stage visibility of the push-constant
                                   * block (FRAGMENT; terrain: VERTEX too) —
                                   * rhi_push_constants must match the range */
};

struct rhi_texture {
    VkImage         image;
    VkDeviceMemory  mem;
    VkImageView     view;
    VkDescriptorSet set;       /* points at (view, dev->vram_sampler) */
    rhi_buffer     *staging;   /* host-visible upload buffer (NULL for render targets) */
    /* Per-frame staging arena for IN-FRAME uploads (rhi_update_vram/rgba8
     * called between rhi_begin_frame and the first render pass): the copy is
     * recorded into the frame's own command buffer and executes at submit,
     * so the payload must stay untouched until then and the previous frame
     * may still be reading its own slot — one arena per frame in flight,
     * created lazily on first in-frame update. Within one frame every upload
     * to this texture takes a DISJOINT range (bump cursor, reset when the
     * frame serial moves on): two uploads in one frame each keep their bytes,
     * where a single reused slot let the second memcpy overwrite the first
     * payload before the GPU copied it. A chunk that cannot hold a request
     * is left in place and a larger one is appended; chunks are never freed
     * mid-life, so a range recorded earlier this frame stays valid.
     * `staging` stays the one-shot (init-time, blocking) path. */
    struct rhi_stage_chunk *stage_arena[RHI_FRAMES_IN_FLIGHT];
    uint64_t        stage_serial[RHI_FRAMES_IN_FLIGHT];  /* frame serial the arena's cursors belong to */
    uint32_t        width, height;
    uint32_t        bpp;       /* bytes per pixel (2 = R16_UINT VRAM, 4 = RGBA8) */
    VkImageLayout   layout;    /* current layout, for re-upload transitions */

    /* Render-target extras (is_rt only): a depth attachment + a framebuffer so
     * geometry can be rendered into `image` via rt_pass (rt_rp, or rt_ds_rp for a
     * depth-sampled target). */
    bool            is_rt;
    VkImage         rt_depth_image;
    VkDeviceMemory  rt_depth_mem;
    VkImageView     rt_depth_view;
    VkFramebuffer   rt_fb;
    VkRenderPass    rt_pass;   /* pass rhi_begin_render_target uses (NULL -> rt_rp) */
    /* Depth-sampled RT only (rhi_create_render_target_depth_sampled): a DEPTH-
     * aspect-only view (sampling a view with DEPTH|STENCIL aspects is illegal)
     * + a descriptor set (sample view, vram_sampler) so the depth can be bound
     * as a combined image sampler. NULL on a plain render target. */
    VkImageView     rt_depth_sample_view;
    VkDescriptorSet depth_set;
    bool            depth_readable; /* depth image carries TRANSFER_SRC
                                     * (rhi_read_render_target_depth) */
    rhi_depth_class depth_class; /* the depth convention of its pass (SCENE default) */
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

/* The Khronos validation layer and its debug-utils messenger are opt-in:
 * OPENPETE_VK_VALIDATION=1 requests them, and only when the loader offers
 * the layer (it ships with the Vulkan SDK, not with drivers). Unset or 0
 * requests neither, so a machine with the SDK installed runs the same
 * instance as one without it. Read once, at instance creation. */
static bool want_validation_layer(void)
{
    const char *e = getenv("OPENPETE_VK_VALIDATION");
    if (!(e && *e && *e != '0')) return false;
    if (layer_available("VK_LAYER_KHRONOS_validation")) return true;
    RHI_LOG("OPENPETE_VK_VALIDATION set but VK_LAYER_KHRONOS_validation is "
            "not installed; validation off\n");
    return false;
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

    bool want_validation = want_validation_layer();

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

/* Stencil-capable depth format (W2.6): prefer D32_SFLOAT_S8 (keeps the float
 * depth curve the depth-model audit analysed), fall back to D24_UNORM_S8. The
 * spec guarantees at least one of the two supports DEPTH_STENCIL_ATTACHMENT
 * with optimal tiling. */
static VkFormat pick_depth_format(VkPhysicalDevice phys)
{
    const VkFormat cand[2] = { VK_FORMAT_D32_SFLOAT_S8_UINT,
                               VK_FORMAT_D24_UNORM_S8_UINT };
    /* OPENPETE_RHI_DEPTH_D24=1 starts at the fallback, so the D24 selection
     * runs on a device that has both. */
    const char *force = getenv("OPENPETE_RHI_DEPTH_D24");
    for (int i = (force && *force == '1') ? 1 : 0; i < 2; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(phys, cand[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return cand[i];
    }
    return VK_FORMAT_D32_SFLOAT_S8_UINT;  /* unreachable per spec */
}

static const char *depth_format_name(VkFormat f)
{
    return f == VK_FORMAT_D32_SFLOAT_S8_UINT ? "D32_SFLOAT_S8_UINT"
         : f == VK_FORMAT_D24_UNORM_S8_UINT  ? "D24_UNORM_S8_UINT" : "other";
}

/* The depth convention per class for the chosen format. FORWARD images
 * store nearer as smaller: LESS_OR_EQUAL, clear 1. The SCENE convention on
 * the float format is reversed (the scene stages write z_ndc = 1/vz,
 * psx_viewz.glsl op_scene_clip): GREATER_OR_EQUAL, clear 0, so the float's
 * exponent tracks 1/vz and every distance keeps about 2^-24 relative
 * precision. D24_UNORM has one absolute step everywhere, where 1/vz would
 * spend it near the camera, so the fallback keeps the forward convention and
 * z_ndc = 1 - 1/vz. */
static void depth_convention_init(rhi_device *dev)
{
    const bool rev = dev->depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    for (int c = 0; c < RHI_DEPTH_CLASSES; c++) {
        dev->depth_cmp[c]   = VK_COMPARE_OP_LESS_OR_EQUAL;
        dev->depth_clear[c] = 1.0f;
        dev->depth_rev[c]   = VK_FALSE;
    }
    if (rev) {
        dev->depth_cmp[RHI_DEPTH_SCENE]   = VK_COMPARE_OP_GREATER_OR_EQUAL;
        dev->depth_clear[RHI_DEPTH_SCENE] = 0.0f;
        dev->depth_rev[RHI_DEPTH_SCENE]   = VK_TRUE;
    }
    dev->cur_depth_clear = dev->depth_clear[RHI_DEPTH_SCENE];
    RHI_LOG("depth format %s\n", depth_format_name(dev->depth_format));
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
            /* Vendor-encoded driverVersion, decoded the way each vendor packs
             * it. A pacing baseline is invalidated by a driver update as
             * surely as by a different GPU, so this is recorded, not guessed. */
            {
                const uint32_t dv = props.driverVersion;
                if (props.vendorID == 0x10DE)          /* NVIDIA: 10.8.8.6 */
                    snprintf(dev->driver_version, sizeof(dev->driver_version),
                             "%u.%u.%u.%u", (dv >> 22) & 0x3FF, (dv >> 14) & 0xFF,
                             (dv >> 6) & 0xFF, dv & 0x3F);
                else
                    snprintf(dev->driver_version, sizeof(dev->driver_version),
                             "%u.%u.%u", VK_API_VERSION_MAJOR(dv),
                             VK_API_VERSION_MINOR(dv), VK_API_VERSION_PATCH(dv));
            }
            dev->device_software = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU);
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
    dev->depth_format = pick_depth_format(chosen);
    depth_convention_init(dev);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = chosen_family,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    /* Optional features (enable only what's supported; nothing here is
     * load-bearing): depthBiasClamp lets rhi_set_depth_bias bound the decal
     * pull (W2.5) — without it the clamp param is forced to 0 (spec
     * VUID-vkCmdSetDepthBias-depthBiasClamp-00790). */
    VkPhysicalDeviceFeatures supported;
    vkGetPhysicalDeviceFeatures(dev->phys, &supported);
    VkPhysicalDeviceFeatures enable = {0};
    enable.depthBiasClamp = supported.depthBiasClamp;
    dev->has_depth_bias_clamp = (supported.depthBiasClamp == VK_TRUE);
    /* shaderFloat64: the retained-terrain vertex stage mirrors the CPU
     * walk's double-precision GTE math bit-for-bit (float32 would flip
     * raster edges at the last-ulp level and break the pixel-identical
     * gate). Universally present on discrete GPUs and lavapipe; the
     * terrain cache simply stays disabled without it. */
    enable.shaderFloat64 = supported.shaderFloat64;
    dev->has_shader_float64 = (supported.shaderFloat64 == VK_TRUE);
    /* fillModeNonSolid: VK_POLYGON_MODE_LINE for the wireframe view
     * (rhi_set_wireframe); without it that view falls back to filled. */
    enable.fillModeNonSolid = supported.fillModeNonSolid;
    dev->has_fill_non_solid = (supported.fillModeNonSolid == VK_TRUE);
    /* Optional display-timing pair. present_id tags each present with a
     * monotonically increasing id; present_wait blocks until the present
     * carrying that id has actually been DISPLAYED. Together they are the only
     * portable way to learn a flip time — every timestamp the engine takes
     * itself is a SUBMIT time, and under FIFO the display quantises submits to
     * its own refresh, so submit-side spacing can look even while the flips do
     * not (and the reverse). Enabled only when both are present; nothing here
     * is load-bearing. */
    uint32_t dev_ext_n = 0;
    vkEnumerateDeviceExtensionProperties(dev->phys, NULL, &dev_ext_n, NULL);
    VkExtensionProperties *dev_avail = dev_ext_n
        ? calloc(dev_ext_n, sizeof(*dev_avail)) : NULL;
    if (dev_avail)
        vkEnumerateDeviceExtensionProperties(dev->phys, NULL, &dev_ext_n, dev_avail);
    bool have_present_id = false, have_present_wait = false;
    for (uint32_t i = 0; i < dev_ext_n && dev_avail; i++) {
        if (strcmp(dev_avail[i].extensionName, "VK_KHR_present_id") == 0)
            have_present_id = true;
        else if (strcmp(dev_avail[i].extensionName, "VK_KHR_present_wait") == 0)
            have_present_wait = true;
    }
    free(dev_avail);

    /* OFF unless asked for, because enabling it is NOT free: chaining a
     * present id onto every present measured 34.5 / 31.0 / 33.4 late ticks per
     * 1000 against 11.7 / 12.9 / 15.2 with the pair declined — same leg, same
     * binary, complete separation, and the enabled arm fails the standing
     * gate's threshold of 15. An extension that changes what the driver does
     * with the present queue has to be priced before it is switched on for
     * everyone, not assumed inert because the app ignores it.
     *
     * OPENPETE_PACE_FLIP_LOG implies it: asking for flip times is asking to
     * pay for them, and the pacing numbers from such a run are not comparable
     * with a normal one. */
    const char *pt_env = getenv("OPENPETE_PRESENT_TIMING");
    const char *fl_env = getenv("OPENPETE_PACE_FLIP_LOG");
    const char *lt_env = getenv("OPENPETE_EAGER_LATTICE");
    /* OPENPETE_EAGER_LATTICE implies it too: there the presenter is its own
     * thread, the sim thread never touches the queue, and the flips are the
     * pacing SOURCE (the present thread fits the display's vblank lattice
     * from them). OPENPETE_PRESENT_TIMING=0 declines it explicitly. */
    const bool pt_declined = pt_env && *pt_env == '0';
    const bool pt_want = !pt_declined &&
                         ((pt_env && *pt_env && *pt_env != '0') ||
                          (fl_env && *fl_env && *fl_env != '0') ||
                          (lt_env && *lt_env && *lt_env != '0'));
    RHI_LOG("display timing: present_id %s, present_wait %s -> %s\n",
            have_present_id ? "available" : "absent",
            have_present_wait ? "available" : "absent",
            (pt_want && have_present_id && have_present_wait)
                ? "ENABLED (costs pacing; measurement runs only)" : "off");
    dev->has_present_timing = pt_want && !dev->headless &&
                              have_present_id && have_present_wait;
    const char *dev_ext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                              "VK_KHR_present_id", "VK_KHR_present_wait" };
    VkPhysicalDevicePresentWaitFeaturesKHR feat_wait = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
        .presentWait = VK_TRUE,
    };
    VkPhysicalDevicePresentIdFeaturesKHR feat_id = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
        .pNext = &feat_wait,
        .presentId = VK_TRUE,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = dev->has_present_timing ? (const void *)&feat_id : NULL,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        /* No swapchain extension when there's no surface to present to. */
        .enabledExtensionCount = dev->headless ? 0u
                                 : (dev->has_present_timing ? 3u : 1u),
        .ppEnabledExtensionNames = dev->headless ? NULL : dev_ext,
        .pEnabledFeatures = &enable,
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

/* True if the surface advertises `want`. FIFO is always supported (spec) so
 * callers only need to query the vsync-OFF modes. */
static bool surface_has_present_mode(rhi_device *dev, VkPresentModeKHR want)
{
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev->phys, dev->surface, &n, NULL);
    VkPresentModeKHR *modes = calloc(n ? n : 1, sizeof(*modes));
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev->phys, dev->surface, &n, modes);
    bool ok = false;
    for (uint32_t i = 0; i < n; i++) if (modes[i] == want) { ok = true; break; }
    free(modes);
    return ok;
}

/* Swapchain present mode. Precedence: the explicit --vsync flag (dev->vsync_pref
 * 0|1) beats the OPENPETE_PRESENT_MODE env, which beats the default FIFO (vsync
 * ON — always supported, locks present to the monitor refresh).
 *   --vsync 1        -> FIFO (vsync ON).
 *   --vsync 0        -> a vsync-OFF mode (immediate, else mailbox) for low latency
 *                       or uncapped-throughput benchmarking (FIFO otherwise hides
 *                       the real frame rate behind the refresh ceiling).
 *   --vsync unset    -> OPENPETE_PRESENT_MODE=immediate|mailbox if set, else FIFO.
 * A vsync-OFF request the surface lacks falls back to FIFO (with a log). */
static VkPresentModeKHR choose_present_mode(rhi_device *dev)
{
    if (dev->vsync_pref == 1)
        return VK_PRESENT_MODE_FIFO_KHR;               /* explicit vsync ON */

    if (dev->vsync_pref == 0) {                         /* explicit vsync OFF */
        if (surface_has_present_mode(dev, VK_PRESENT_MODE_IMMEDIATE_KHR)) {
            RHI_LOG("present mode: immediate (--vsync 0)\n");
            return VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
        if (surface_has_present_mode(dev, VK_PRESENT_MODE_MAILBOX_KHR)) {
            RHI_LOG("present mode: mailbox (--vsync 0; immediate unsupported)\n");
            return VK_PRESENT_MODE_MAILBOX_KHR;
        }
        RHI_LOG("--vsync 0 but no vsync-OFF present mode supported; using FIFO (vsync)\n");
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    /* unset (-1): defer to the env override, else default FIFO (vsync ON). */
    const char *env = getenv("OPENPETE_PRESENT_MODE");
    VkPresentModeKHR want = VK_PRESENT_MODE_FIFO_KHR;
    if (env && !strcmp(env, "immediate")) want = VK_PRESENT_MODE_IMMEDIATE_KHR;
    else if (env && !strcmp(env, "mailbox")) want = VK_PRESENT_MODE_MAILBOX_KHR;
    if (want == VK_PRESENT_MODE_FIFO_KHR) return want; /* always supported */
    if (!surface_has_present_mode(dev, want)) {
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
        .presentMode = (dev->present_mode_granted = choose_present_mode(dev)), /* FIFO (vsync) unless --vsync 0 / OPENPETE_PRESENT_MODE */
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

    /* Budget = two refresh periods, floored at RHI_ACQUIRE_TIMEOUT_NS and
     * capped well under the pacer's slip threshold.
     *
     * TWO periods, not one: presenting below the refresh rate means the image
     * arrives at a repaint boundary, and a budget of exactly one period
     * expires on any jitter at all. The cap keeps an expiry from becoming a
     * sim slip; the floor covers an unknown or absurd reported refresh.
     *
     * Only load-bearing under FIFO — a non-blocking mode never waits — but a
     * mode change recreates the swapchain, so this re-runs either way. */
    {
        float hz = 0.0f;
        if (dev->window) {
            const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(
                SDL_GetDisplayForWindow(dev->window));
            if (dm) hz = dm->refresh_rate;
        }
        dev->refresh_hz = hz;
        /* One period is what the display owes us; the half is jitter margin.
         * Scaling with the refresh rather than flooring everything at a fixed
         * value matters in BOTH directions: a 60 Hz display needs 25 ms or it
         * expires constantly, and a 480 Hz display gets 4 ms — TIGHTER than
         * the 8 ms this replaces, so a genuine stall is abandoned sooner on
         * the machine that can least afford to wait. */
        Uint64 budget = (hz > 1.0f) ? (Uint64)(1.5f * 1e9f / hz)
                                    : RHI_ACQUIRE_TIMEOUT_NS;
        if (budget < RHI_ACQUIRE_BUDGET_MIN_NS) budget = RHI_ACQUIRE_BUDGET_MIN_NS;
        if (budget > RHI_ACQUIRE_BUDGET_MAX_NS) budget = RHI_ACQUIRE_BUDGET_MAX_NS;
        dev->acquire_budget_ns = budget;
        RHI_LOG("swapchain: %u image(s), acquire budget %.1f ms (refresh %.3f Hz)\n",
                dev->sc_count, (double)budget / 1e6, (double)hz);
    }
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
        .format = dev->depth_format,      /* combined depth+stencil (W2.6) */
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, /* not read after the pass */
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,   /* stencil starts 0 every pass */
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

    /* LOAD variant (SCENE postfx point, S5): identical attachments so it
     * shares off_fb, but the color attachment LOADs its existing contents
     * (initialLayout COLOR_ATTACHMENT — the caller barriers off_image
     * TRANSFER_SRC → COLOR before begin) instead of clearing, so HUD draws
     * land ON TOP of a scene that a SCENE-point chain already post-
     * processed. Depth still CLEARs (HUD self-clears depth anyway; the
     * scene pass discarded it). Only the external→subpass color-acquire
     * dependency differs (LOAD reads the attachment), so keep the same
     * dep set; drop the WAR dep's relevance by keeping it (harmless). */
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentDescription atts_load[2] = { color, depth };
    rci.pAttachments = atts_load;
    r = vkCreateRenderPass(dev->device, &rci, NULL, &dev->off_load_rp);
    if (r != VK_SUCCESS) { RHI_LOG("vkCreateRenderPass (load) failed: %d\n", (int)r); return false; }
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
        .format = dev->depth_format,      /* combined depth+stencil (W2.6) */
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,   /* stencil starts 0 every pass */
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

/* Render pass for DEPTH-SAMPLED render targets (the shadow-decal depth
 * pre-pass): identical attachment formats to rt_rp (pipeline-compatible), but
 * the depth attachment is STORED and finishes in SHADER_READ_ONLY_OPTIMAL so a
 * later pass in the same frame can sample the per-pixel depth. Dependencies
 * add: (a) acquire waits on the PREVIOUS frame's fragment-shader reads of the
 * depth (WAR — the image is re-rendered every present); (b) release makes the
 * depth writes visible to fragment sampling. */
static bool create_rt_ds_render_pass(rhi_device *dev)
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
        .format = dev->depth_format,      /* combined depth+stencil (W2.6) */
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,        /* SAMPLED after the pass */
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
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
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,   /* WAR vs last frame's sampling */
          .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
          .srcAccessMask = 0,
          .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT },
        { /* subpass -> external: make color AND depth results sample-visible */
          .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT },
    };
    VkAttachmentDescription atts[2] = { color, depth };
    VkRenderPassCreateInfo rci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2, .pAttachments = atts,
        .subpassCount = 1, .pSubpasses = &sub,
        .dependencyCount = 2, .pDependencies = deps,
    };
    VkResult r = vkCreateRenderPass(dev->device, &rci, NULL, &dev->rt_ds_rp);
    if (r != VK_SUCCESS) { RHI_LOG("vkCreateRenderPass(rt-ds) failed: %d\n", (int)r); return false; }
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
        /* SAMPLED: the COMPOSITE postfx point reads the composed frame
         * (rhi_offscreen_to_sampled); TRANSFER_DST: the chain's final
         * ping blits back in (rhi_blit_rt_to_offscreen) — S2/S3. */
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
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
    /* Depth attachment (3a.3): combined depth+stencil, device-local. */
    VkImageCreateInfo dici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = dev->depth_format,
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
        .format = dev->depth_format,
        .subresourceRange = {
            /* attachment view of a combined format must cover both aspects */
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
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
    /* New image/view ⇒ the per-slot off_sampled_set descriptors are stale;
     * rhi_bind_offscreen_tex rewrites a slot's set when its gen lags. */
    dev->off_gen++;
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
    RHI_LOG("frames in flight = %d\n", RHI_FRAMES_IN_FLIGHT);
    for (int i = 0; i < RHI_FRAMES_IN_FLIGHT; i++) {
        VK_CHECK(vkCreateSemaphore(dev->device, &sem, NULL, &dev->img_avail[i]));
        VK_CHECK(vkCreateFence(dev->device, &fence, NULL, &dev->in_flight[i]));
    }
    for (uint32_t i = 0; i < dev->sc_count; i++) {
        VK_CHECK(vkCreateSemaphore(dev->device, &sem, NULL, &dev->render_done[i]));
    }
    /* The staged present step's own command buffers and sync. */
    VkCommandBufferAllocateInfo pcbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = dev->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = RHI_STAGE_RING,
    };
    if (vkAllocateCommandBuffers(dev->device, &pcbi, dev->pcmd) != VK_SUCCESS) {
        RHI_LOG("vkAllocateCommandBuffers (present step) failed\n");
        return false;
    }
    for (int i = 0; i < RHI_STAGE_RING; i++) {
        VK_CHECK(vkCreateSemaphore(dev->device, &sem, NULL, &dev->pimg_avail[i]));
        VK_CHECK(vkCreateFence(dev->device, &fence, NULL, &dev->pfence[i]));
    }
    return true;
}

/* GPU per-pass timestamp zones — created only when OPENPETE_GPU_TIMERS=1 AND
 * the graphics queue supports timestamps; otherwise the zone calls compile to
 * a flag check. Unsupported hardware disables with one log line. */
static bool create_gpu_timers(rhi_device *dev)
{
    const char *e = getenv("OPENPETE_GPU_TIMERS");
    if (!(e && *e && *e != '0')) return true;   /* off: zero cost */

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(dev->phys, &props);
    uint32_t qn = 0, valid_bits = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev->phys, &qn, NULL);
    if (qn > 0) {
        VkQueueFamilyProperties *q = calloc(qn, sizeof(*q));
        if (!q) return true;
        vkGetPhysicalDeviceQueueFamilyProperties(dev->phys, &qn, q);
        if (dev->gfx_family < qn) valid_bits = q[dev->gfx_family].timestampValidBits;
        free(q);
    }
    if (valid_bits == 0 || props.limits.timestampPeriod <= 0.0f) {
        RHI_LOG("OPENPETE_GPU_TIMERS: no timestamp support on the graphics queue — disabled\n");
        return true;
    }
    dev->gpu_ts_period = props.limits.timestampPeriod;

    VkQueryPoolCreateInfo qi = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = RHI_GPU_ZONES_MAX * 2,
    };
    for (int i = 0; i < RHI_FRAMES_IN_FLIGHT; i++)
        VK_CHECK(vkCreateQueryPool(dev->device, &qi, NULL, &dev->gpu_qp[i]));
    dev->gpu_timers = true;
    RHI_LOG("GPU pass timers on (%.3f ns/tick)\n", (double)dev->gpu_ts_period);
    return true;
}

/* Pull the finished timestamps for the slot rhi_begin_frame just fenced.
 * Never blocks: the in_flight fence proves the slot's queries completed. */
static void gpu_timers_harvest(rhi_device *dev)
{
    int slot = (int)dev->frame;
    int n = dev->gpu_zone_n[slot];
    dev->gpu_results_n = 0;
    dev->gpu_zone_n[slot] = 0;
    if (n <= 0) return;
    uint64_t ts[RHI_GPU_ZONES_MAX * 2];
    VkResult r = vkGetQueryPoolResults(dev->device, dev->gpu_qp[slot], 0,
                                       (uint32_t)(n * 2),
                                       sizeof(uint64_t) * (size_t)(n * 2), ts,
                                       sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS) return;    /* shouldn't happen post-fence; drop the frame */
    /* Frame anchor = earliest begin timestamp, so start_us places every zone
     * on one per-present timeline the trace can lay out. */
    uint64_t t_min = ts[0];
    for (int i = 0; i < n; i++) if (ts[i * 2] < t_min) t_min = ts[i * 2];
    for (int i = 0; i < n; i++) {
        dev->gpu_results[i].name  = dev->gpu_zone_name[slot][i];
        dev->gpu_results[i].depth = dev->gpu_zone_depth[slot][i];
        dev->gpu_results[i].us =
            (double)(ts[i * 2 + 1] - ts[i * 2]) * (double)dev->gpu_ts_period / 1000.0;
        dev->gpu_results[i].start_us =
            (double)(ts[i * 2] - t_min) * (double)dev->gpu_ts_period / 1000.0;
    }
    dev->gpu_results_n = n;
    dev->gpu_results_token = dev->gpu_zone_token[slot];
}

int rhi_gpu_timers_on(const rhi_device *dev)
{
    return dev && dev->gpu_timers;
}

/* s_last_gpu_zone (declared beside rhi_signal_device_lost, which prints
 * it): the device-lost suspect — populated only when timers are on. */
void rhi_gpu_zone_begin(rhi_device *dev, const char *name)
{
    if (!dev || !dev->gpu_timers || !dev->frame_active) return;
    int slot = (int)dev->frame;
    if (dev->gpu_zone_n[slot] >= RHI_GPU_ZONES_MAX ||
        dev->gpu_zone_sp >= RHI_GPU_ZONES_MAX) return;
    int idx = dev->gpu_zone_n[slot]++;          /* reserve the query pair now  */
    s_last_gpu_zone = name;
    dev->gpu_zone_name[slot][idx]  = name;
    dev->gpu_zone_depth[slot][idx] = (uint8_t)dev->gpu_zone_sp;
    dev->gpu_zone_stack[dev->gpu_zone_sp++] = idx;
    vkCmdWriteTimestamp(dev->cmd[slot], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        dev->gpu_qp[slot], (uint32_t)(idx * 2));
}

void rhi_gpu_zone_end(rhi_device *dev)
{
    if (!dev || !dev->gpu_timers || !dev->frame_active || dev->gpu_zone_sp <= 0) return;
    int slot = (int)dev->frame;
    int idx = dev->gpu_zone_stack[--dev->gpu_zone_sp];
    vkCmdWriteTimestamp(dev->cmd[slot], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        dev->gpu_qp[slot], (uint32_t)(idx * 2 + 1));
}

int rhi_gpu_zone_results(rhi_device *dev, rhi_gpu_zone_result *out, int cap)
{
    if (!dev || !out || cap <= 0) return 0;
    int n = dev->gpu_results_n < cap ? dev->gpu_results_n : cap;
    memcpy(out, dev->gpu_results, (size_t)n * sizeof(*out));
    return n;
}

void rhi_gpu_frame_token(rhi_device *dev, uint64_t token)
{
    if (!dev || !dev->gpu_timers || !dev->frame_active) return;
    dev->gpu_zone_token[(int)dev->frame] = token;
}

uint64_t rhi_gpu_results_token(const rhi_device *dev)
{
    return dev ? dev->gpu_results_token : 0;
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
    /* LINEAR variant for high-res replacement art (rhi_texture_set_filter_linear)
     * — same addressing, filtered mag/min. */
    si.magFilter = VK_FILTER_LINEAR; si.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(dev->device, &si, NULL, &dev->linear_sampler) != VK_SUCCESS) {
        RHI_LOG("vkCreateSampler (linear) failed\n"); return false;
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
    VkDescriptorSetLayoutBinding pb[2] = { b, b };
    pb[1].binding = 1;
    VkDescriptorSetLayoutCreateInfo pli = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = pb,
    };
    if (vkCreateDescriptorSetLayout(dev->device, &pli, NULL, &dev->pair_set_layout) != VK_SUCCESS) {
        RHI_LOG("vkCreateDescriptorSetLayout (pair) failed\n"); return false;
    }
    /* S2: dynamic-UBO pair layout (postfx set 3: binding 0 = engine block,
     * binding 1 = mod block), vertex+fragment visible. */
    VkDescriptorSetLayoutBinding ub[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
    };
    VkDescriptorSetLayoutCreateInfo uli = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = ub,
    };
    if (vkCreateDescriptorSetLayout(dev->device, &uli, NULL, &dev->ubo_set_layout) != VK_SUCCESS) {
        RHI_LOG("vkCreateDescriptorSetLayout (ubo) failed\n"); return false;
    }
    /* Single dynamic-UBO binding, for engine blocks bound at a set index of
     * their own (rhi_ubo_bind_at) rather than inside the contract set. */
    VkDescriptorSetLayoutCreateInfo u1li = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = ub,
    };
    if (vkCreateDescriptorSetLayout(dev->device, &u1li, NULL,
                                    &dev->ubo1_set_layout) != VK_SUCCESS) {
        RHI_LOG("vkCreateDescriptorSetLayout (ubo1) failed\n"); return false;
    }
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev->phys, &props);
        dev->ubo_align = props.limits.minUniformBufferOffsetAlignment;
        if (dev->ubo_align == 0) dev->ubo_align = 256;
        dev->max_desc_sets = props.limits.maxBoundDescriptorSets;
    }

    /* Every rhi_texture (the VRAM display texture, RTT targets, AND every HD
     * texture-pack replacement) permanently holds one COMBINED_IMAGE_SAMPLER
     * set for its lifetime. A whole-level HD pack loads one texture per unique
     * replacement image as surfaces come into view — hundreds for a level like
     * Artisans — so the old 32/48 cap exhausted the pool after ~32 textures,
     * and the failed per-frame re-allocation retries collapsed the frame rate.
     * Sized for a large per-level pack; sets are a few hundred bytes each, so
     * the headroom is cheap. (A growable multi-pool would remove the ceiling
     * entirely — future work if a pack ever exceeds this.) */
    VkDescriptorPoolSize ps[2] = {
        { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 4096 },
        /* S2: off_image sampled sets (per frame slot) + rhi_ubo pairs. */
        { .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .descriptorCount = 64 },
    };
    VkDescriptorPoolCreateInfo pi = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 4160, .poolSizeCount = 2, .pPoolSizes = ps,
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

static void destroy_stages(rhi_device *dev)
{
    for (int i = 0; i < RHI_STAGE_RING; i++) {
        if (dev->stage_image[i]) { vkDestroyImage(dev->device, dev->stage_image[i], NULL); dev->stage_image[i] = VK_NULL_HANDLE; }
        if (dev->stage_mem[i])   { vkFreeMemory(dev->device, dev->stage_mem[i], NULL); dev->stage_mem[i] = VK_NULL_HANDLE; }
        dev->stage_layout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    dev->stage_gen++;
}

static bool create_stages(rhi_device *dev)
{
    for (int i = 0; i < RHI_STAGE_RING; i++) {
        VkImageCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = dev->sc_format,
            .extent = { dev->sc_extent.width, dev->sc_extent.height, 1 },
            .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        if (vkCreateImage(dev->device, &ici, NULL, &dev->stage_image[i]) != VK_SUCCESS) {
            RHI_LOG("stage vkCreateImage failed\n"); return false;
        }
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(dev->device, dev->stage_image[i], &mr);
        uint32_t mt = find_memtype(dev->phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt == UINT32_MAX) { RHI_LOG("no device-local memory type\n"); return false; }
        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size, .memoryTypeIndex = mt,
        };
        if (vkAllocateMemory(dev->device, &mai, NULL, &dev->stage_mem[i]) != VK_SUCCESS) {
            RHI_LOG("stage vkAllocateMemory failed\n"); return false;
        }
        vkBindImageMemory(dev->device, dev->stage_image[i], dev->stage_mem[i], 0);
        dev->stage_layout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    return true;
}

static void destroy_swapchain_resources(rhi_device *dev)
{
    destroy_stages(dev);
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
    /* Present ids belong to a swapchain. The new one starts its sequence over,
     * and any id still outstanding against the old one can never complete. */
    dev->present_id_next = 1;
    dev->present_id_submitted = 0;
    dev->present_id_seen = 0;
    /* The acquire semaphores and slot fences start over too. A binary
     * acquire semaphore can be left ARMED with no waiter (an acquire whose
     * frame was refused or superseded before its submit), and Vulkan has no
     * way to un-signal one: handing it to vkAcquireNextImageKHR on the new
     * swapchain is invalid reuse of a signaled semaphore. Likewise a slot
     * fence can be left UNSIGNALED with nothing pending (reset after an
     * acquire whose submit never happened), which would park the next wait
     * on it for its full timeout. After the device-idle above nothing
     * references any of them, so recreating the lot restores the freshly
     * created state every acquire/wait path assumes. Any acquire still held
     * died with its swapchain. */
    {
        VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fi = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        for (int i = 0; i < RHI_FRAMES_IN_FLIGHT; i++) {
            vkDestroySemaphore(dev->device, dev->img_avail[i], NULL);
            VK_CHECK(vkCreateSemaphore(dev->device, &si, NULL, &dev->img_avail[i]));
            vkDestroyFence(dev->device, dev->in_flight[i], NULL);
            VK_CHECK(vkCreateFence(dev->device, &fi, NULL, &dev->in_flight[i]));
        }
        for (int i = 0; i < RHI_STAGE_RING; i++) {
            vkDestroySemaphore(dev->device, dev->pimg_avail[i], NULL);
            VK_CHECK(vkCreateSemaphore(dev->device, &si, NULL, &dev->pimg_avail[i]));
            vkDestroyFence(dev->device, dev->pfence[i], NULL);
            VK_CHECK(vkCreateFence(dev->device, &fi, NULL, &dev->pfence[i]));
        }
        dev->pacquired = false;
    }
    destroy_swapchain_resources(dev);
    if (!create_swapchain(dev)) return false;   /* also nulls images_in_flight */
    if (!create_offscreen(dev)) return false;
    if (!create_stages(dev)) return false;
    VkSemaphoreCreateInfo sem = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < dev->sc_count; i++)
        VK_CHECK(vkCreateSemaphore(dev->device, &sem, NULL, &dev->render_done[i]));
    dev->needs_recreate = false;
    return true;
}

/* ------------------------------------------------------------------ */

/* The device's memory table, one line per type: which properties a type
 * carries and which heap it draws from. A harness that must name a heap
 * reads it here instead of inferring it from an allocation's type index. */
static void rhi_log_memory_types(rhi_device *dev)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(dev->phys, &mp);
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
        RHI_LOG("memory heap %u: %llu MiB flags %x\n", i,
                (unsigned long long)(mp.memoryHeaps[i].size >> 20),
                (unsigned)mp.memoryHeaps[i].flags);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        RHI_LOG("memory type %u: flags %x heap %u\n", i,
                (unsigned)mp.memoryTypes[i].propertyFlags,
                mp.memoryTypes[i].heapIndex);
}

/* First memory type drawing from `heap`, or -1 when the heap backs none. */
static int balloon_heap_memtype(const VkPhysicalDeviceMemoryProperties *mp,
                                uint32_t heap)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if (mp->memoryTypes[i].heapIndex == heap) return (int)i;
    return -1;
}

/* Allocates RHI_BALLOON_BLOCK_MIB blocks on `type` until `want_blocks` are
 * held, or, when `want_blocks` is 0, until the driver refuses or this spec's
 * own block cap is reached; then releases
 * whole blocks totalling at least `release_mib`. Blocks are neither mapped
 * nor bound to a buffer. Returns the driver's refusal code, VK_SUCCESS when
 * none occurred, and writes the block count this call still holds. */
static VkResult balloon_hold(rhi_device *dev, uint32_t type, int want_blocks,
                             unsigned release_mib, int *out_blocks)
{
    const int first = dev->balloon_count;
    int cap = first + RHI_BALLOON_SPEC_CAP;
    if (cap > RHI_BALLOON_MAX_BLOCKS) cap = RHI_BALLOON_MAX_BLOCKS;
    VkResult r = VK_SUCCESS;
    while (dev->balloon_count < cap) {
        if (want_blocks > 0 && dev->balloon_count - first >= want_blocks) break;
        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = (VkDeviceSize)RHI_BALLOON_BLOCK_MIB << 20,
            .memoryTypeIndex = type,
        };
        VkDeviceMemory m = VK_NULL_HANDLE;
        r = vkAllocateMemory(dev->device, &mai, NULL, &m);
        if (r != VK_SUCCESS) break;
        dev->balloon_blocks[dev->balloon_count++] = m;
    }
    if (dev->balloon_count >= cap)
        RHI_LOG("balloon: block cap %d reached\n", cap - first);
    for (unsigned freed = 0; freed < release_mib && dev->balloon_count > first;
         freed += RHI_BALLOON_BLOCK_MIB)
        vkFreeMemory(dev->device, dev->balloon_blocks[--dev->balloon_count], NULL);
    *out_blocks = dev->balloon_count - first;
    return r;
}

/* One balloon spec: `<heap>:<MiB>` holds that many MiB, `<heap>:fill` holds
 * until the driver refuses, `<heap>:fill-<MiB>` fills and then releases at
 * least <MiB>, leaving a margin of free space in the heap. A heap that does
 * not exist, or that backs no memory type, is reported and skipped. */
static void balloon_spec(rhi_device *dev,
                         const VkPhysicalDeviceMemoryProperties *mp,
                         const char *spec)
{
    char *end = NULL;
    unsigned long heap = strtoul(spec, &end, 10);
    if (end == spec || *end != ':') {
        RHI_LOG("balloon: spec '%s' is not <heap>:<MiB>|<heap>:fill[-<MiB>]\n", spec);
        return;
    }
    if (heap >= mp->memoryHeapCount) {
        RHI_LOG("balloon: heap %lu does not exist (%u heaps)\n", heap,
                mp->memoryHeapCount);
        return;
    }
    const int type = balloon_heap_memtype(mp, (uint32_t)heap);
    if (type < 0) { RHI_LOG("balloon: heap %lu backs no memory type\n", heap); return; }

    const char *amount = end + 1;
    int want_blocks = 0;
    unsigned release_mib = 0;
    if (strncmp(amount, "fill", 4) == 0)
        release_mib = amount[4] == '-' ? (unsigned)strtoul(amount + 5, NULL, 10) : 0u;
    else
        want_blocks = (int)((strtoul(amount, NULL, 10) + RHI_BALLOON_BLOCK_MIB - 1)
                            / RHI_BALLOON_BLOCK_MIB);

    int blocks = 0;
    VkResult r = balloon_hold(dev, (uint32_t)type, want_blocks, release_mib, &blocks);
    RHI_LOG("balloon heap %u type %u: %u MiB held in %d blocks, refusal %d\n",
            (unsigned)heap, (unsigned)type,
            (unsigned)blocks * RHI_BALLOON_BLOCK_MIB, blocks, (int)r);
}

/* OPENPETE_RHI_BALLOON=<spec>[,<spec>...] holds device memory so that every
 * allocation made after device creation in the named heaps is refused by the
 * driver, which is the condition rhi_create_vertex_buffer_device's memory-type
 * retry exists for. A heap filled here is a fixed condition for the life of
 * the process; the `fill-<MiB>` margin form covers a heap whose free space
 * varies, where an allocation that fits early fails later. */
static void rhi_balloon_init(rhi_device *dev)
{
    const char *env = getenv("OPENPETE_RHI_BALLOON");
    if (!env || !*env) return;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(dev->phys, &mp);
    for (const char *p = env; *p; ) {
        char spec[64];
        size_t n = strcspn(p, ",");
        size_t keep = n < sizeof spec ? n : sizeof spec - 1;
        memcpy(spec, p, keep);
        spec[keep] = '\0';
        balloon_spec(dev, &mp, spec);
        p += n;
        if (*p == ',') p++;
    }
}

/* Releases every block the balloon holds. Called while the device is alive. */
static void rhi_balloon_free(rhi_device *dev)
{
    for (int i = 0; i < dev->balloon_count; i++)
        vkFreeMemory(dev->device, dev->balloon_blocks[i], NULL);
    dev->balloon_count = 0;
}

static rhi_device *rhi_create_impl(struct SDL_Window *window, const char *app_name,
                                   bool headless, uint32_t hl_w, uint32_t hl_h,
                                   int vsync)
{
    rhi_device *dev = calloc(1, sizeof(*dev));
    if (!dev) return NULL;
    dev->window     = window;
    dev->headless   = headless;
    dev->vsync_pref = vsync;   /* consulted by choose_present_mode (windowed only) */

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
    if (!create_rt_ds_render_pass(dev)) goto fail;
    if (!create_offscreen(dev))         goto fail;
    if (!headless && !create_stages(dev)) goto fail;
    if (!create_sync_and_commands(dev)) goto fail;
    if (!create_descriptor_infra(dev))  goto fail;
    if (!create_gpu_timers(dev))        goto fail;

    rhi_log_memory_types(dev);
    /* After the RHI's own images, buffers and staging rings are resident, so a
     * filled heap refuses only what the renderer allocates from here on. */
    rhi_balloon_init(dev);

    RHI_LOG("device='%s'%s format=%d %ux%u images=%u\n",
            dev->device_name, headless ? " (headless)" : "", (int)dev->sc_format,
            dev->sc_extent.width, dev->sc_extent.height, dev->sc_count);
    return dev;

fail:
    rhi_destroy(dev);
    return NULL;
}

rhi_device *rhi_create(struct SDL_Window *window, const char *app_name, int vsync)
{
    rhi_display_claim(NULL);   /* boot: the creating thread presents until a handoff */
    return rhi_create_impl(window, app_name, /*headless=*/false, 0, 0, vsync);
}

rhi_device *rhi_create_headless(const char *app_name, uint32_t width, uint32_t height)
{
    rhi_display_claim(NULL);
    /* Surfaceless: no swapchain, so present mode / vsync is irrelevant (-1). */
    return rhi_create_impl(NULL, app_name, /*headless=*/true, width, height, -1);
}

bool rhi_is_headless(const rhi_device *dev)
{
    return dev && dev->headless;
}

void rhi_destroy(rhi_device *dev)
{
    if (ring_gen_dbg())
        fprintf(stderr, "[rhi/vk] ring generations: bumps=%llu overflows=%llu vb_gen_writes=%llu vb_gen_binds=%llu\n",
                (unsigned long long)s_ring_gen_bumps, (unsigned long long)s_ring_gen_overflows,
                (unsigned long long)s_dbg_gw, (unsigned long long)s_dbg_gb);
    if (!dev) return;
    if (dev->device) { vkDeviceWaitIdle(dev->device); rhi_balloon_free(dev); retired_reclaim(dev, true); }

    if (dev->readback_buf) { rhi_destroy_buffer(dev, dev->readback_buf); dev->readback_buf = NULL; }
    for (int i = 0; i < RHI_FRAMES_IN_FLIGHT; i++) {
        if (dev->readback_ring[i].staging) {
            rhi_destroy_buffer(dev, dev->readback_ring[i].staging);
            dev->readback_ring[i].staging = NULL;
        }
        if (dev->grid_ring[i].staging) {
            rhi_destroy_buffer(dev, dev->grid_ring[i].staging);
            dev->grid_ring[i].staging = NULL;
        }
    }
    destroy_swapchain_resources(dev);
    if (dev->off_rp) vkDestroyRenderPass(dev->device, dev->off_rp, NULL);
    if (dev->off_load_rp) vkDestroyRenderPass(dev->device, dev->off_load_rp, NULL);
    if (dev->rt_rp)  vkDestroyRenderPass(dev->device, dev->rt_rp, NULL);
    if (dev->rt_ds_rp) vkDestroyRenderPass(dev->device, dev->rt_ds_rp, NULL);

    if (dev->desc_pool)      vkDestroyDescriptorPool(dev->device, dev->desc_pool, NULL);
    if (dev->vram_set_layout) vkDestroyDescriptorSetLayout(dev->device, dev->vram_set_layout, NULL);
    if (dev->ubo_set_layout) vkDestroyDescriptorSetLayout(dev->device, dev->ubo_set_layout, NULL);
    if (dev->ubo1_set_layout) vkDestroyDescriptorSetLayout(dev->device, dev->ubo1_set_layout, NULL);
    vkDestroyDescriptorSetLayout(dev->device, dev->pair_set_layout, NULL);  /* null: no-op */
    if (dev->vram_sampler)   vkDestroySampler(dev->device, dev->vram_sampler, NULL);
    if (dev->linear_sampler) vkDestroySampler(dev->device, dev->linear_sampler, NULL);

    for (int i = 0; i < RHI_STAGE_RING; i++) {
        if (dev->pimg_avail[i]) vkDestroySemaphore(dev->device, dev->pimg_avail[i], NULL);
        if (dev->pfence[i])     vkDestroyFence(dev->device, dev->pfence[i], NULL);
    }
    for (int i = 0; i < RHI_FRAMES_IN_FLIGHT; i++) {
        if (dev->img_avail[i]) vkDestroySemaphore(dev->device, dev->img_avail[i], NULL);
        if (dev->in_flight[i]) vkDestroyFence(dev->device, dev->in_flight[i], NULL);
        if (dev->gpu_qp[i])    vkDestroyQueryPool(dev->device, dev->gpu_qp[i], NULL);
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

bool rhi_frame_active(const rhi_device *dev)
{
    return dev && dev->frame_active;
}

uint64_t rhi_frame_serial(rhi_device *dev) { (void)dev; return s_frame_serial; }

/* Report any present-path phase that blocks long enough to threaten the sim.
 * Under the thread split the presenting thread is the sim thread, so a stall
 * here is a dropped tick — and "which call blocked" is otherwise invisible. */
#define RHI_STALL_REPORT_NS (12ull * 1000ull * 1000ull)

/* Block time per present-path phase, accumulated for the pacing log. The
 * >=12 ms report catches catastrophes; the pacing plan needs the whole
 * distribution, including the sub-millisecond FIFO wait that IS the pacing on
 * a vsynced present. Statics rather than device fields: exactly one thread
 * presents, and the consumer (the host loop) has no rhi_device handle. */
static Uint64 s_block_ns[RHI_BLOCK_PHASE_COUNT];

static void rhi_note_stall_phase(const char *what, int phase, Uint64 t0)
{
    Uint64 dt = SDL_GetTicksNS() - t0;
    if (phase >= 0) s_block_ns[phase] += dt;
    if (dt >= RHI_STALL_REPORT_NS)
        RHI_LOG("stall: %s blocked %.1f ms\n", what, (double)dt / 1e6);
}

uint64_t rhi_fence_sheds(const rhi_device *dev)
{
    return dev ? dev->fence_sheds : 0;
}

uint64_t rhi_acquire_sheds(const rhi_device *dev)
{
    return dev ? dev->acquire_sheds : 0;
}

uint64_t rhi_acquire_timeouts(const rhi_device *dev)
{
    return dev ? dev->acquire_timeouts : 0;
}

uint32_t rhi_swapchain_image_count(const rhi_device *dev)
{
    return dev ? dev->sc_count : 0;
}

void rhi_take_block_times(int64_t *out, int n)
{
    for (int i = 0; i < n && i < RHI_BLOCK_PHASE_COUNT; i++) {
        out[i] = (int64_t)s_block_ns[i];
        s_block_ns[i] = 0;
    }
}

const char *rhi_driver_version(const rhi_device *dev)
{
    return (dev && dev->driver_version[0]) ? dev->driver_version : "unknown";
}

const char *rhi_present_mode_name(const rhi_device *dev)
{
    if (!dev) return "none";
    switch (dev->present_mode_granted) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "immediate";
    case VK_PRESENT_MODE_MAILBOX_KHR:      return "mailbox";
    case VK_PRESENT_MODE_FIFO_KHR:         return "fifo";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo-relaxed";
    default:                               return "other";
    }
}

float rhi_refresh_hz(const rhi_device *dev) { return dev ? dev->refresh_hz : 0.0f; }
bool rhi_present_is_vsync(const rhi_device *dev)
{
    return dev && !dev->headless && (dev->present_mode_granted == VK_PRESENT_MODE_FIFO_KHR ||
                                     dev->present_mode_granted == VK_PRESENT_MODE_FIFO_RELAXED_KHR);
}

void rhi_set_present_blocking(rhi_device *dev, bool on)
{
    if (dev) dev->present_blocking = on;
}

bool rhi_needs_recreate(const rhi_device *dev)
{
    return dev && dev->needs_recreate;
}

/* Acquire a swapchain image on `sem`, then wait out the frame that last
 * used it. 1 = acquired (dev->image_index); 0 = refused (timeout, rebuild
 * pending, device lost), nothing consumed unless noted in the counters. */
static int acquire_image(rhi_device *dev, VkSemaphore sem, VkFence *owner_fence)
{
    {
        Uint64 acq_t0 = SDL_GetTicksNS();
        VkResult acq = vkAcquireNextImageKHR(dev->device, dev->swapchain,
                                             dev->acquire_budget_ns
                                                 ? dev->acquire_budget_ns
                                                 : RHI_ACQUIRE_TIMEOUT_NS,
                                             sem,
                                             VK_NULL_HANDLE, &dev->image_index);
        rhi_note_stall_phase("acquire", RHI_BLOCK_ACQUIRE, acq_t0);
        if (acq == VK_TIMEOUT || acq == VK_NOT_READY) {
            /* No image within the budget: drop this present, keep the sim
             * running. The semaphore was not signalled, so nothing waits on
             * it and the next frame re-acquires cleanly. */
            dev->acquire_timeouts++;
            dev->acquire_sheds++;
            if (!dev->present_blocking)
                dev->acquire_backoff_until_ns = SDL_GetTicksNS() + RHI_ACQUIRE_BACKOFF_NS;
            return 0;
        }
        if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
            dev->needs_recreate = true;
            return 0;
        } else if (acq == VK_ERROR_DEVICE_LOST) {
            rhi_signal_device_lost("acquire");
            return 0;
        } else if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
            RHI_LOG("vkAcquireNextImageKHR failed: %d\n", (int)acq);
            return 0;
        }

        /* If a previous frame is still using this image, wait on its fence. */
        if (dev->images_in_flight[dev->image_index] != VK_NULL_HANDLE) {
            /* Bounded, and TAGGED: this wait was previously invisible to the
             * block-time census, so attribution that blamed the slot fence
             * could have been under-counting it. Not skippable — acquire has
             * already consumed img_avail by this point, and returning here
             * would leave that semaphore signalled with nothing to wait on
             * it. In practice it is almost always already signalled once the
             * poll above has passed, so the bound is a backstop. */
            Uint64 iif_t0 = SDL_GetTicksNS();
            VkResult iw = vkWaitForFences(dev->device, 1, &dev->images_in_flight[dev->image_index],
                                          VK_TRUE, dev->headless ? UINT64_MAX
                                                 : dev->present_blocking ? RHI_BLOCKING_FENCE_TIMEOUT_NS
                                                 : RHI_IMAGE_FENCE_TIMEOUT_NS);
            rhi_note_stall_phase("image-fence", RHI_BLOCK_IMAGE_FENCE, iif_t0);
            if (iw != VK_SUCCESS) {
                /* The image is still owned by a frame in flight and the
                 * acquire semaphore is already consumed: the only clean exit
                 * is a rebuild, which waits the device idle and starts the
                 * semaphores over. Recording against the image would be a
                 * device loss. */
                RHI_LOG("image fence timed out (%.1f ms): rebuilding the swapchain\n",
                        (double)(SDL_GetTicksNS() - iif_t0) / 1e6);
                dev->needs_recreate = true;
                return 0;
            }
        }
        dev->images_in_flight[dev->image_index] = *owner_fence;
    }
    return 1;
}

/* The swapchain, rebuilt if it must be (rate-limited, and never mid-drag).
 * False = no usable swapchain this time; the caller skips its present. */
static bool ensure_swapchain(rhi_device *dev)
{
    if (dev->needs_recreate && !dev->headless) {
        /* Rate-limit. A live resize reports OUT_OF_DATE on every frame, and
         * each recreate is a vkDeviceWaitIdle plus a full swapchain teardown
         * and rebuild — doing that per frame stalls the sim for as long as the
         * user drags the edge. Retry on an interval and skip the present in
         * between; the last attempt before the size settles is the one that
         * sticks. */
        /* Do not rebuild while the window is still being dragged. Presenting
         * against a stale extent reports OUT_OF_DATE every frame, so without
         * this the rate-limit below just paces a 40-50 ms teardown-and-rebuild
         * forever. Skipping the present costs nothing the user can see during
         * a resize, and keeps the sim off this call entirely. */
        int cw = 0, ch = 0;
        if (!dev->resize_discrete &&
            SDL_GetWindowSizeInPixels(dev->window, &cw, &ch) && cw > 0 && ch > 0) {
            if (cw != dev->last_seen_w || ch != dev->last_seen_h) {
                dev->last_seen_w = cw;
                dev->last_seen_h = ch;
                dev->size_stable_since_ns = SDL_GetTicksNS();
                return false;
            }
            if (SDL_GetTicksNS() - dev->size_stable_since_ns < RHI_RESIZE_SETTLE_NS)
                return false;
        }
        if (dev->recreate_backoff_until_ns &&
            SDL_GetTicksNS() < dev->recreate_backoff_until_ns)
            return false;
        Uint64 t0 = SDL_GetTicksNS();
        bool ok = recreate(dev);
        if (ok && dev->resize_discrete) {
            /* Consumed. A later OUT_OF_DATE (the compositor delivering the
             * size in two steps) rebuilds again through the normal path. */
            dev->resize_discrete = false;
            SDL_GetWindowSizeInPixels(dev->window, &dev->last_seen_w,
                                      &dev->last_seen_h);
            dev->size_stable_since_ns = SDL_GetTicksNS();
        }
        rhi_note_stall_phase("recreate", RHI_BLOCK_RECREATE, t0);
        dev->recreate_backoff_until_ns = SDL_GetTicksNS() + RHI_ACQUIRE_BACKOFF_NS;
        if (!ok) return false;
    }
    return true;
}

/* Free the replaced buffer allocations no frame can still read. A frame
 * RHI_FRAMES_IN_FLIGHT serials back has had its fence waited by the time the
 * current serial began, so an allocation replaced at serial R is free once the
 * serial reaches R + RHI_FRAMES_IN_FLIGHT. `all` frees every entry; the caller
 * has waited the device idle. */
static void retired_reclaim(rhi_device *dev, bool all)
{
    struct rhi_retired_buf **pp = &dev->retired;
    while (*pp) {
        struct rhi_retired_buf *r = *pp;
        if (!all && r->serial + RHI_FRAMES_IN_FLIGHT > s_frame_serial) { pp = &r->next; continue; }
        *pp = r->next;
        vkUnmapMemory(dev->device, r->memory);
        vkFreeMemory(dev->device, r->memory, NULL);
        vkDestroyBuffer(dev->device, r->buffer, NULL);
        free(r);
    }
}

static bool begin_frame_impl(rhi_device *dev, bool acquire)
{
    seam_check("begin_frame");
    if (!ensure_swapchain(dev)) return false;

    {
        /* Do not BLOCK on the slot's previous submission — poll it.
         *
         * Under FIFO this fence retires at the display's consumption rate, so
         * waiting on it is waiting for a vblank. Whichever thread presents is
         * then paced by the monitor, and that thread also runs the sim:
         * measured on a 480 Hz display at the shipped default, 47.8 % of all
         * wall time was spent inside this one call, against 0.1 % at the same
         * present count with vsync off. The sim clock cannot be correct while
         * the display owns it.
         *
         * Skipping is safe HERE specifically: this sits before
         * vkAcquireNextImageKHR, so nothing has been consumed and no
         * semaphore is left pending. Every caller already tolerates a false
         * return — the bounded-acquire path established that contract for the
         * same reason.
         *
         * Headless keeps the blocking wait: there is no display to pace it,
         * and its readback path depends on the slot having retired.
         *
         * The queue self-paces to exactly the display rate either way; the
         * difference is whether the excess is absorbed by shedding a present
         * or by stalling the game. */
        Uint64 t0 = SDL_GetTicksNS();
        if (dev->headless) {
            vkWaitForFences(dev->device, 1, &dev->in_flight[dev->frame],
                            VK_TRUE, UINT64_MAX);
        } else if (dev->present_blocking) {
            /* A dedicated present thread is paced by the display by design:
             * waiting here costs the sim nothing. Bounded so a dead
             * compositor still returns control — and the bound is HONOURED:
             * proceeding past a timed-out wait resets a fence and re-records
             * a command buffer the GPU is still executing, which is a
             * device loss a few submits later (seen on a 60 Hz display after
             * a fullscreen toggle, where a rebuild pushed one wait past it). */
            VkResult fw = vkWaitForFences(dev->device, 1, &dev->in_flight[dev->frame],
                                          VK_TRUE, RHI_BLOCKING_FENCE_TIMEOUT_NS);
            if (fw != VK_SUCCESS) {
                dev->fence_sheds++;
                rhi_note_stall_phase("fence-wait", RHI_BLOCK_FENCE, t0);
                return false;
            }
        } else if (vkGetFenceStatus(dev->device,
                                    dev->in_flight[dev->frame]) != VK_SUCCESS) {
            dev->fence_sheds++;
            rhi_note_stall_phase("fence-poll", RHI_BLOCK_FENCE, t0);
            return false;
        }
        rhi_note_stall_phase("fence-wait", RHI_BLOCK_FENCE, t0);
    }

    /* This slot's previous submission has fully retired — its timestamp
     * queries are ready for a block-free harvest. */
    if (dev->gpu_timers) gpu_timers_harvest(dev);

    /* Backing off after a failed acquire: skip the present without blocking,
     * so a compositor that is not handing out images cannot pace the sim. */
    if (!dev->headless && !dev->present_blocking && dev->acquire_backoff_until_ns) {
        if (SDL_GetTicksNS() < dev->acquire_backoff_until_ns) {
            dev->acquire_sheds++;
            return false;
        }
        dev->acquire_backoff_until_ns = 0;
    }

    /* Headless: no swapchain image to acquire — the command buffer records
     * directly into the offscreen / render-target images. */
    if (!dev->headless) {
        /* Bounded, not UINT64_MAX. Under FIFO this normally returns within a
         * vblank, but Windows stops handing out images for the whole of a live
         * resize (the modal loop inside DefWindowProc). An unbounded wait here
         * parks whichever thread presents — and that thread also runs the sim,
         * so the game froze for as long as the user held the window edge.
         * Timing out lets the caller skip the present and keep ticking. */
        if (acquire &&
            !acquire_image(dev, dev->img_avail[dev->frame], &dev->in_flight[dev->frame])) return false;
    }
    vkResetFences(dev->device, 1, &dev->in_flight[dev->frame]);

    VkCommandBuffer cb = dev->cmd[dev->frame];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cb, &bi);
    /* The frame serial counts frames that actually BEGAN — slot fence
     * retired, image acquired, command buffer recording. Counting refused
     * begins broke the promise scene retirement is built on ("a frame
     * RHI_FRAMES_IN_FLIGHT serials back has had its fence waited"): a
     * refusal burst could advance the serial without ever observing the
     * oldest slot's fence, retiring scenes and freeing arena ranges a
     * still-executing submission reads. Every counted begin is followed by
     * its submit, so slot alternation is exact and the promise holds. */
    s_frame_serial++;
    retired_reclaim(dev, false);
    dev->pair_used[dev->frame] = 0;   /* the slot's pair sets have retired */
    dev->frame_active = true;
    dev->draw_calls = 0;
    if (dev->gpu_timers) {
        /* Reset must be recorded outside a render pass — here, before any. */
        vkCmdResetQueryPool(cb, dev->gpu_qp[dev->frame], 0, RHI_GPU_ZONES_MAX * 2);
        dev->gpu_zone_sp = 0;   /* fresh nesting stack for this frame */
    }
    return true;
}

static void image_barrier(VkCommandBuffer cb, VkImage img,
                          VkImageLayout from, VkImageLayout to,
                          VkAccessFlags src_access, VkAccessFlags dst_access,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage);

void rhi_begin_offscreen(rhi_device *dev, float r, float g, float b, float a)
{
    if (!dev->frame_active) return;
    dev->clear[0] = r; dev->clear[1] = g; dev->clear[2] = b; dev->clear[3] = a;
    VkClearValue cv[2];
    cv[0].color.float32[0] = r; cv[0].color.float32[1] = g;
    cv[0].color.float32[2] = b; cv[0].color.float32[3] = a;
    cv[1].depthStencil.depth = dev->depth_clear[RHI_DEPTH_SCENE]; cv[1].depthStencil.stencil = 0; /* far */
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
    dev->cur_depth_clear = cv[1].depthStencil.depth;

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
    /* Belt-only: a defined value if a depth_bias pipeline is somehow bound
     * first in the pass. NOT a guarantee — binding any static-state pipeline
     * invalidates dynamic depth bias, so callers must set it after every
     * depth_bias pipeline bind (see rhi.h). */
    vkCmdSetDepthBias(dev->cmd[dev->frame], 0.0f, 0.0f, 0.0f);
}

/* Re-begin the offscreen pass LOADing off_image's existing contents
 * (SCENE postfx point, S5): after a SCENE-point chain post-processed the
 * scene and left off_image in TRANSFER_SRC, transition it back to a
 * color attachment and resume drawing (the HUD) over it. Depth CLEARs
 * (the HUD clears depth itself). Pairs with rhi_end_offscreen. */
void rhi_begin_offscreen_load(rhi_device *dev)
{
    if (!dev->frame_active) return;
    image_barrier(dev->cmd[dev->frame], dev->off_image,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkClearValue cv[2];
    memset(&cv[0], 0, sizeof cv[0]);   /* color LOADs; value ignored */
    cv[1].depthStencil.depth = dev->depth_clear[RHI_DEPTH_SCENE]; cv[1].depthStencil.stencil = 0;
    VkRenderPassBeginInfo rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = dev->off_load_rp,
        .framebuffer = dev->off_fb,
        .renderArea = { .offset = {0, 0}, .extent = dev->sc_extent },
        .clearValueCount = 2,
        .pClearValues = cv,
    };
    vkCmdBeginRenderPass(dev->cmd[dev->frame], &rp, VK_SUBPASS_CONTENTS_INLINE);
    dev->cur_extent = dev->sc_extent;
    dev->cur_depth_clear = cv[1].depthStencil.depth;
    VkViewport vp = {
        .x = 0.0f, .y = (float)dev->sc_extent.height,
        .width = (float)dev->sc_extent.width, .height = -(float)dev->sc_extent.height,
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
    seam_check("end_frame");
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

    /* Ids start at 1 and must strictly increase per swapchain; a rebuild
     * restarts the sequence, which rhi_present_wait handles by refusing to
     * wait on an id from a swapchain that no longer exists. */
    const uint64_t this_id = dev->present_id_next;
    VkPresentIdKHR pid = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
        .swapchainCount = 1,
        .pPresentIds = &this_id,
    };
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = dev->has_present_timing ? (const void *)&pid : NULL,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &dev->render_done[dev->image_index],
        .swapchainCount = 1,
        .pSwapchains = &dev->swapchain,
        .pImageIndices = &dev->image_index,
    };
    Uint64 pres_t0 = SDL_GetTicksNS();
    VkResult pr = vkQueuePresentKHR(dev->gfx_queue, &pi);
    if (dev->has_present_timing && pr >= 0) {
        dev->present_id_submitted = this_id;
        dev->present_id_next++;
    }
    rhi_note_stall_phase("present", RHI_BLOCK_PRESENT, pres_t0);
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

bool rhi_begin_frame(rhi_device *dev) { return begin_frame_impl(dev, true); }

/* ---- staged presents ------------------------------------------------ */

/* A frame that will end in a stage image acquires no swapchain image: an
 * acquire whose semaphore is never waited on would leave it signalled for
 * the next acquire, and the images would run out (measured: every render
 * refused after the third). */
bool rhi_begin_frame_staged(rhi_device *dev)
{
    if (dev->headless) return false;
    return begin_frame_impl(dev, false);
}

int rhi_stage_count(void) { return RHI_STAGE_RING; }
uint32_t rhi_stage_gen(const rhi_device *dev) { return dev ? dev->stage_gen : 0; }

void rhi_end_frame_to_stage(rhi_device *dev, int stage)
{
    seam_check("end_frame_to_stage");
    if (!dev->frame_active) return;
    if (dev->headless || stage < 0 || stage >= RHI_STAGE_RING || !dev->stage_image[stage]) {
        rhi_end_frame(dev);
        return;
    }
    VkCommandBuffer cb = dev->cmd[dev->frame];
    VkImage st = dev->stage_image[stage];
    /* Stage image -> transfer dst. Its previous reader was a present step's
     * blit, in an earlier submission on this queue: the barrier's TRANSFER
     * source stage covers that read (barriers order against every prior
     * submission, not only this command buffer). */
    image_barrier(cb, st, dev->stage_layout[stage], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  dev->stage_layout[stage] == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_TRANSFER_READ_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT,
                  dev->stage_layout[stage] == VK_IMAGE_LAYOUT_UNDEFINED
                      ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit region = {
        .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .srcOffsets = { {0,0,0}, {(int32_t)dev->sc_extent.width, (int32_t)dev->sc_extent.height, 1} },
        .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .dstOffsets = { {0,0,0}, {(int32_t)dev->sc_extent.width, (int32_t)dev->sc_extent.height, 1} },
    };
    vkCmdBlitImage(cb, dev->off_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   st, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
    /* And back to transfer src, ready for the present step's blit. */
    image_barrier(cb, st, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    dev->stage_layout[stage] = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkEndCommandBuffer(cb);
    /* No swapchain image was acquired for this frame, so nothing to wait on
     * or signal: the fence alone gates the slot's reuse. */
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
    };
    VK_CHECK(vkQueueSubmit(dev->gfx_queue, 1, &si, dev->in_flight[dev->frame]));
    dev->frame = (dev->frame + 1) % RHI_FRAMES_IN_FLIGHT;
    dev->frame_active = false;
}

bool rhi_present_stage_acquire(rhi_device *dev)
{
    seam_check("present_stage_acquire");
    if (dev->headless || dev->frame_active) return false;
    if (dev->pacquired) return true;
    if (!ensure_swapchain(dev)) return false;
    const uint32_t p = dev->pslot;
    /* This ring slot's previous present step must have retired before its
     * command buffer is re-recorded. A dedicated presenter blocks here by
     * design; the bound is honoured (a stale re-record is a device loss). */
    Uint64 t0 = SDL_GetTicksNS();
    VkResult fw = vkWaitForFences(dev->device, 1, &dev->pfence[p], VK_TRUE, RHI_BLOCKING_FENCE_TIMEOUT_NS);
    if (fw != VK_SUCCESS) { dev->fence_sheds++; rhi_note_stall_phase("stage-fence", RHI_BLOCK_FENCE, t0); return false; }
    rhi_note_stall_phase("stage-fence", RHI_BLOCK_FENCE, t0);
    if (!acquire_image(dev, dev->pimg_avail[p], &dev->pfence[p])) return false;
    vkResetFences(dev->device, 1, &dev->pfence[p]);
    dev->pacquired = true;
    dev->pacquired_gen = dev->stage_gen;
    dev->pacquired_index = dev->image_index;
    return true;
}

bool rhi_present_stage_submit(rhi_device *dev, int stage)
{
    seam_check("present_stage_submit");
    if (dev->headless || dev->frame_active) return false;
    if (stage < 0 || stage >= RHI_STAGE_RING) return false;
    if (!dev->pacquired) { if (!rhi_present_stage_acquire(dev)) return false; }
    /* The acquire belongs to the swapchain it was made on: a rebuild in
     * between (stage_gen moved) waited the device idle and started the
     * semaphores over, so the held image is gone with it. */
    if (dev->pacquired_gen != dev->stage_gen) { dev->pacquired = false; return false; }
    if (!dev->stage_image[stage] || dev->stage_layout[stage] != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        return false;                    /* never filled, or rebuilt since; the acquire stays held */
    }
    dev->pacquired = false;
    const uint32_t p = dev->pslot;
    dev->image_index = dev->pacquired_index;
    VkCommandBuffer cb = dev->pcmd[p];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cb, &bi);
    VkImage sc = dev->sc_images[dev->image_index];
    image_barrier(cb, sc, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit region = {
        .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .srcOffsets = { {0,0,0}, {(int32_t)dev->sc_extent.width, (int32_t)dev->sc_extent.height, 1} },
        .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .dstOffsets = { {0,0,0}, {(int32_t)dev->sc_extent.width, (int32_t)dev->sc_extent.height, 1} },
    };
    vkCmdBlitImage(cb, dev->stage_image[stage], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
    image_barrier(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(cb);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &dev->pimg_avail[p],
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &dev->render_done[dev->image_index],
    };
    VK_CHECK(vkQueueSubmit(dev->gfx_queue, 1, &si, dev->pfence[p]));
    dev->pslot = (p + 1) % RHI_STAGE_RING;

    const uint64_t this_id = dev->present_id_next;
    VkPresentIdKHR pid = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
        .swapchainCount = 1,
        .pPresentIds = &this_id,
    };
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = dev->has_present_timing ? (const void *)&pid : NULL,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &dev->render_done[dev->image_index],
        .swapchainCount = 1,
        .pSwapchains = &dev->swapchain,
        .pImageIndices = &dev->image_index,
    };
    Uint64 pres_t0 = SDL_GetTicksNS();
    VkResult pr = vkQueuePresentKHR(dev->gfx_queue, &pi);
    if (dev->has_present_timing && pr >= 0) {
        dev->present_id_submitted = this_id;
        dev->present_id_next++;
    }
    rhi_note_stall_phase("present", RHI_BLOCK_PRESENT, pres_t0);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        dev->needs_recreate = true;
    } else if (pr == VK_ERROR_DEVICE_LOST) {
        rhi_signal_device_lost("present");
        return false;
    } else if (pr != VK_SUCCESS) {
        RHI_LOG("vkQueuePresentKHR failed: %d\n", (int)pr);
        return false;
    }
    return true;
}

bool rhi_present_stage(rhi_device *dev, int stage)
{
    return rhi_present_stage_acquire(dev) && rhi_present_stage_submit(dev, stage);
}

/* Give back an image held by rhi_present_stage_acquire whose frame will
 * never be submitted. Vulkan has no un-acquire: the only clean exits for
 * an acquired image are a present or a swapchain rebuild. Present it with
 * whatever it already holds -- a frame the display showed a few refreshes
 * ago, on screen for one refresh -- which consumes the armed semaphore and
 * leaves the swapchain whole for the next owner of the display. Without
 * this, a handoff to the inline path strands one armed acquire per
 * handoff, later submits pair that stale semaphore with images it never
 * covered, and the driver faults the process after a few cycles. */
bool rhi_present_stage_release(rhi_device *dev)
{
    seam_check("present_stage_release");
    if (!dev || dev->headless || !dev->pacquired) return true;
    dev->pacquired = false;
    if (dev->pacquired_gen != dev->stage_gen) return true;  /* rebuilt: acquire died with the swapchain */
    const uint32_t p = dev->pslot;
    dev->image_index = dev->pacquired_index;
    VkCommandBuffer cb = dev->pcmd[p];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cb, &bi);
    /* PRESENT_SRC -> PRESENT_SRC via UNDEFINED keeps whatever pixels the
     * image last held; no draw, no blit. */
    image_barrier(cb, dev->sc_images[dev->image_index], VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &dev->pimg_avail[p],
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &dev->render_done[dev->image_index],
    };
    VK_CHECK(vkQueueSubmit(dev->gfx_queue, 1, &si, dev->pfence[p]));
    dev->pslot = (p + 1) % RHI_STAGE_RING;
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &dev->render_done[dev->image_index],
        .swapchainCount = 1,
        .pSwapchains = &dev->swapchain,
        .pImageIndices = &dev->image_index,
    };
    /* No VkPresentIdKHR on this present: it is not a frame anyone waits on,
     * and the id sequence stays with the real submits. */
    VkResult pr = vkQueuePresentKHR(dev->gfx_queue, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) dev->needs_recreate = true;
    else if (pr == VK_ERROR_DEVICE_LOST) { rhi_signal_device_lost("stage-release"); return false; }
    return true;
}

int rhi_present_timing_supported(const rhi_device *dev)
{
    return dev && dev->has_present_timing ? 1 : 0;
}

int rhi_present_outstanding(const rhi_device *dev)
{
    if (!dev || !dev->has_present_timing || !dev->swapchain) return 0;
    return (int)(dev->present_id_submitted - dev->present_id_seen);
}

int rhi_present_wait(rhi_device *dev, uint64_t timeout_ns, uint64_t *out_id,
                     uint64_t *out_waited_ns)
{
    seam_check("present_wait");
    if (!dev || !dev->has_present_timing || !dev->swapchain) return -1;
    if (dev->needs_recreate) return -1;

    /* A timeout under about a millisecond is answered VK_SUCCESS at once for a
     * present that has NOT been displayed (measured: every first ask after a
     * present, at 0, 1 and 50 us). This function is therefore only called
     * with millisecond-scale timeouts, and never as a poll. */
    /* Confirm ids IN ORDER, oldest first, even on a dedicated presenter that
     * only ever wants the newest: a wait issued on the newest id while an
     * older one was still unconfirmed came back with 0.5 ms-quantised return
     * times (residual to the vblank lattice ~160 us against ~20 us), every
     * time, on the same driver. Confirming the older ids first -- each an
     * instant return -- keeps the wait that matters on its event path. */
    /* Confirm ids in order, and keep going while they come back INSTANTLY.
     *
     * Waiting on the newest present is useless — under FIFO it is one to three
     * refreshes from the screen, past any single sleep budget, so it times out
     * nearly every time. Waiting on the oldest unconfirmed one is right, but
     * one confirmation per call is slower than presents are queued (measured
     * 337/s confirmed against 423/s presented), so the backlog grows and every
     * answer is a frame that flipped long ago.
     *
     * Draining fixes both: an id that has already flipped returns with no
     * blocking, costing a driver call, and the loop stops at the first id that
     * has NOT — which is the next real flip, and the only one whose return
     * time means anything. The drain is bounded so a long backlog cannot turn
     * one measurement into unbounded work on the sim thread. */
    /* Small: every call costs a driver round-trip even when the id is already
     * signalled, and inline (non-eager) this runs on the sim thread. */
    enum { RHI_FLIP_DRAIN_MAX = 8 };
    const Uint64 t_start = SDL_GetTicksNS();
    for (int i = 0; i < RHI_FLIP_DRAIN_MAX; i++) {
        const uint64_t id = dev->present_id_seen + 1;
        if (id > dev->present_id_submitted) return -1;
        const Uint64 elapsed = SDL_GetTicksNS() - t_start;
        if (elapsed >= timeout_ns) return 0;
        const Uint64 t0 = SDL_GetTicksNS();
        VkResult r = vkWaitForPresentKHR(dev->device, dev->swapchain, id,
                                         timeout_ns - elapsed);
        const Uint64 waited = SDL_GetTicksNS() - t0;
        if (r == VK_SUCCESS) {
            dev->present_id_seen = id;
            if (out_id) *out_id = id;
            if (out_waited_ns) *out_waited_ns = (uint64_t)waited;
            /* Blocked, so the caller's clock at return IS the flip. A stale
             * confirmation keeps draining instead. */
            if (waited >= RHI_FLIP_OBSERVED_NS) return 1;
            continue;
        }
        if (r == VK_TIMEOUT) return 0;
        /* SUBOPTIMAL/OUT_OF_DATE arrive here when the surface changed under
         * us; the present path already flags the rebuild, so this only
         * declines. */
        return -1;
    }
    return 0;
}

void rhi_notify_resize(rhi_device *dev)
{
    if (dev) dev->needs_recreate = true;
}

/* As rhi_notify_resize, for a size change the app itself caused: rebuild at
 * the next begin_frame instead of waiting out the settle window meant for a
 * live drag. The back-off is cleared too — a toggle is not the sustained
 * OUT_OF_DATE storm that back-off exists to damp. */
void rhi_notify_resize_immediate(rhi_device *dev)
{
    if (!dev) return;
    dev->needs_recreate = true;
    dev->resize_discrete = true;
    dev->recreate_backoff_until_ns = 0;
    dev->acquire_backoff_until_ns = 0;
}

void rhi_set_vsync(rhi_device *dev, int vsync)
{
    if (!dev) return;
    if (dev->vsync_pref == vsync) return;            /* no change */
    dev->vsync_pref = vsync;                          /* choose_present_mode reads it on recreate */
    if (!dev->headless) dev->needs_recreate = true;  /* rebuild swapchain next begin_frame */
}

int rhi_get_vsync(const rhi_device *dev)
{
    return dev ? dev->vsync_pref : -1;
}

bool rhi_get_imgui_info(rhi_device *dev, rhi_imgui_info *out)
{
    if (!dev || !out || dev->headless) return false;
    out->instance        = (void *)dev->instance;
    out->physical_device = (void *)dev->phys;
    out->device          = (void *)dev->device;
    out->queue_family    = dev->gfx_family;
    out->queue           = (void *)dev->gfx_queue;
    out->render_pass     = (uint64_t)dev->off_rp;
    out->image_count     = dev->sc_count >= 2 ? dev->sc_count : 2;
    out->api_version     = VK_API_VERSION_1_2;   /* matches create_instance */
    return true;
}

void *rhi_current_command_buffer(rhi_device *dev)
{
    if (!dev || !dev->frame_active) return NULL;
    return (void *)dev->cmd[dev->frame];
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

bool rhi_device_is_software(const rhi_device *dev)
{
    return dev ? dev->device_software : false;
}

bool rhi_device_has_float64(const rhi_device *dev)
{
    return dev ? dev->has_shader_float64 : false;
}

bool rhi_device_has_fill_non_solid(const rhi_device *dev)
{
    return dev ? dev->has_fill_non_solid : false;
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
    b->dev = dev;
    return b;
}

rhi_buffer *rhi_create_vertex_buffer(rhi_device *dev, size_t bytes)
{
    return create_host_buffer(dev, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              /*prefer_cached=*/false);
}

/* Memory-type indices OPENPETE_RHI_FAIL_MEMTYPE=<index>[,<index>...] names,
 * as a bit per index, read once. A device offers at most VK_MAX_MEMORY_TYPES
 * (32) types, so one word holds the whole table. An index outside the table
 * and a malformed element are ignored; an unset variable refuses nothing. */
static uint32_t tc_fail_mask(void)
{
    static uint32_t mask;
    static bool parsed;
    if (!parsed) {
        parsed = true;
        const char *s = getenv("OPENPETE_RHI_FAIL_MEMTYPE");
        while (s && *s) {
            char *end = NULL;
            long v = strtol(s, &end, 10);
            if (end == s) break;
            if (v >= 0 && v < (long)VK_MAX_MEMORY_TYPES) mask |= 1u << (unsigned)v;
            if (*end != ',') break;
            s = end + 1;
        }
    }
    return mask;
}

/* One allocation attempt for a device vertex buffer, and the only
 * vkAllocateMemory call site of that path.
 *
 * Every attempt on a memory-type index OPENPETE_RHI_FAIL_MEMTYPE names
 * reports VK_ERROR_OUT_OF_DEVICE_MEMORY without reaching the driver, so the
 * fallback order can be exercised on a device whose heaps do not run out, and
 * a device with more host-visible candidates than a heap balloon can fill can
 * have the remaining candidates refused here. */
static VkResult tc_alloc_try(rhi_device *dev, const VkMemoryAllocateInfo *mai,
                             VkDeviceMemory *out)
{
    if (tc_fail_mask() & (1u << mai->memoryTypeIndex))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    return vkAllocateMemory(dev->device, mai, NULL, out);
}

/* A host-mapped vertex buffer the vertex stage fetches every present (the
 * terrain arenas): written rarely, on sector rebuilds, and read by the GPU
 * continuously.
 *
 * Allocation is attempted on every host-visible, host-coherent memory type
 * the buffer accepts, DEVICE_LOCAL types first (rhi_memtype_candidates), and
 * the first type that allocates wins. A device-local host-visible heap can be
 * far smaller than the device's video memory (a 256 MB BAR window, or
 * whatever a resizable-BAR driver reports), and its free space varies with
 * what else is resident, so an allocation that fits on one run fails on the
 * next; falling through to system memory costs bandwidth and keeps the
 * buffer. NULL is returned only when every candidate fails. */
rhi_buffer *rhi_create_vertex_buffer_device(rhi_device *dev, size_t bytes)
{
    rhi_buffer *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->size = (VkDeviceSize)bytes;
    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(dev->device, &bi, NULL, &b->buffer) != VK_SUCCESS) {
        RHI_LOG("vkCreateBuffer failed\n"); free(b); return NULL;
    }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev->device, b->buffer, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(dev->phys, &mp);
    uint32_t cand[VK_MAX_MEMORY_TYPES];
    int ncand = rhi_memtype_candidates(&mp, mr.memoryTypeBits, cand,
                                       (int)(sizeof cand / sizeof cand[0]));
    VkResult r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    for (int i = 0; i < ncand; i++) {
        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size,
            .memoryTypeIndex = cand[i],
        };
        r = tc_alloc_try(dev, &mai, &b->memory);
        if (r == VK_SUCCESS) {
            RHI_LOG("device vertex buffer %zu B: type %u flags %x\n", bytes,
                    cand[i], (unsigned)mp.memoryTypes[cand[i]].propertyFlags);
            break;
        }
        RHI_LOG("device vertex buffer %zu B: type %u failed (%d)\n", bytes,
                cand[i], (int)r);
    }
    if (r != VK_SUCCESS) {
        RHI_LOG("device vertex buffer %zu B: no memory type of %d allocated\n",
                bytes, ncand);
        vkDestroyBuffer(dev->device, b->buffer, NULL); free(b); return NULL;
    }
    vkBindBufferMemory(dev->device, b->buffer, b->memory, 0);
    vkMapMemory(dev->device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->mapped);
    b->dev = dev;
    return b;
}

rhi_buffer *rhi_create_vertex_buffer_dynamic_gens(rhi_device *dev, size_t bytes,
                                                  uint32_t gens)
{
    if (gens < 1) gens = 1;
    rhi_buffer *b = create_host_buffer(dev, bytes * RHI_FRAMES_IN_FLIGHT * gens,
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                       /*prefer_cached=*/false);
    if (b) { b->ring_stride = (VkDeviceSize)bytes; b->gens = gens; }
    return b;
}

rhi_buffer *rhi_create_vertex_buffer_dynamic(rhi_device *dev, size_t bytes)
{
    return rhi_create_vertex_buffer_dynamic_gens(dev, bytes, 1);
}

size_t rhi_vertex_buffer_slot_bytes(const rhi_buffer *buf)
{
    if (!buf) return 0;
    return (size_t)(buf->ring_stride ? buf->ring_stride : buf->size);
}

bool rhi_vertex_buffer_reserve(rhi_device *dev, rhi_buffer *buf, size_t bytes)
{
    if (!buf) return false;
    if (!buf->ring_stride) return (VkDeviceSize)bytes <= buf->size;
    if ((VkDeviceSize)bytes <= buf->ring_stride) return true;
    seam_check("vertex_buffer_reserve");   /* the retire list is the display owner's */
    VkDeviceSize stride = buf->ring_stride;
    while (stride < (VkDeviceSize)bytes) stride *= 2;
    const uint32_t slots = RHI_FRAMES_IN_FLIGHT * (buf->gens ? buf->gens : 1);
    struct rhi_retired_buf *r = calloc(1, sizeof(*r));
    rhi_buffer *nb = r ? create_host_buffer(dev, (size_t)(stride * slots),
                                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                            /*prefer_cached=*/false)
                       : NULL;
    if (!nb) {
        free(r);
        RHI_LOG("vertex ring buffer: growth to %llu B per slot FAILED; holding %llu B\n",
                (unsigned long long)stride, (unsigned long long)buf->ring_stride);
        return false;
    }
    /* Every slot and generation keeps its bytes: a range written earlier in
     * this frame and bound after the growth reads the same data. */
    for (uint32_t i = 0; i < slots; i++)
        memcpy((uint8_t *)nb->mapped + stride * i,
               (const uint8_t *)buf->mapped + buf->ring_stride * i,
               (size_t)buf->ring_stride);
    r->buffer = buf->buffer; r->memory = buf->memory; r->serial = s_frame_serial;
    r->next = dev->retired; dev->retired = r;
    RHI_LOG("vertex ring buffer grew %llu -> %llu B per slot\n",
            (unsigned long long)buf->ring_stride, (unsigned long long)stride);
    buf->buffer = nb->buffer; buf->memory = nb->memory; buf->mapped = nb->mapped;
    buf->size = nb->size; buf->ring_stride = stride;
    free(nb);
    return true;
}

void rhi_update_buffer_range(rhi_buffer *buf, const void *data,
                             size_t offset, size_t bytes)
{
    if (!buf || !buf->mapped) return;
    if (g_openpete_guest_running) op_gpu_audit_map_write(bytes);
    VkDeviceSize cap = buf->ring_stride ? buf->ring_stride : buf->size;
    if ((VkDeviceSize)offset >= cap) return;
    if ((VkDeviceSize)(offset + bytes) > cap) bytes = (size_t)(cap - offset);
    if (!buf->ring_stride) {
        memcpy((uint8_t *)buf->mapped + offset, data, bytes);
        return;
    }
    if (buf->dev && buf->dev->frame_active) {
        memcpy((uint8_t *)buf->mapped + buf->ring_stride * ring_write_slot(buf) + offset,
               data, bytes);
    } else {
        for (uint32_t i = 0; i < RHI_FRAMES_IN_FLIGHT * (buf->gens ? buf->gens : 1); i++)
            memcpy((uint8_t *)buf->mapped + buf->ring_stride * i + offset,
                   data, bytes);
    }
}

void rhi_update_buffer(rhi_buffer *buf, const void *data, size_t bytes)
{
    if (!buf || !buf->mapped) return;
    if (g_openpete_guest_running) op_gpu_audit_map_write(bytes);
    VkDeviceSize cap = buf->ring_stride ? buf->ring_stride : buf->size;
    if ((VkDeviceSize)bytes > cap) bytes = (size_t)cap;
    if (!buf->ring_stride) {
        memcpy(buf->mapped, data, bytes);
        return;
    }
    if (buf->dev && buf->dev->frame_active) {
        /* In-frame: write only the current frame's slot (its fence was waited
         * in rhi_begin_frame, so no in-flight GPU read can see this write). */
        memcpy((uint8_t *)buf->mapped + buf->ring_stride * ring_write_slot(buf),
               data, bytes);
    } else {
        /* Outside a frame (init / rare re-layout under rhi_wait_idle): fill
         * every slot so any frame parity binds the same content. */
        for (uint32_t i = 0; i < RHI_FRAMES_IN_FLIGHT * (buf->gens ? buf->gens : 1); i++)
            memcpy((uint8_t *)buf->mapped + buf->ring_stride * i, data, bytes);
    }
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

/* A render target's depth image usage: the attachment, sampled for a
 * depth-sampled target, a transfer source for a readable one. */
static VkImageUsageFlags rt_depth_usage(bool depth_sampleable, bool depth_readable)
{
    VkImageUsageFlags u = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (depth_sampleable) u |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (depth_readable)   u |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return u;
}

static rhi_texture *create_render_target_impl(rhi_device *dev, uint32_t width,
                                              uint32_t height, bool depth_sampleable,
                                              bool depth_readable)
{
    if (depth_sampleable) {
        /* SAMPLED_IMAGE is not in the spec's REQUIRED feature set for the
         * combined depth+stencil formats — verify before building a pipeline
         * around sampling it (universally supported in practice; the caller
         * falls back to a plain depth-tested pipe if this ever trips). */
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(dev->phys, dev->depth_format, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            RHI_LOG("depth format %d not sampleable on this device\n", (int)dev->depth_format);
            return NULL;
        }
    }
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
        /* TRANSFER_DST: rhi_snapshot_offscreen_to_rt blits INTO a render
         * target (the freeze backdrop capture). Missing until 2026-07-02 —
         * an illegal blit destination per spec, which real GPUs happened to
         * tolerate (validation off). Suspected then FALSIFIED as the cause of
         * the Linux level-transition hang (that's a recompiled-gameplay-code
         * spin — see the plan doc); kept as a genuine latent-UB fix. */
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
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

    /* Depth attachment for the RT framebuffer (combined depth+stencil). */
    VkImageCreateInfo dici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = dev->depth_format,
        .extent = { width, height, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = rt_depth_usage(depth_sampleable, depth_readable),
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
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = dev->depth_format,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                              .levelCount = 1, .layerCount = 1 },
    };
    if (vkCreateImageView(dev->device, &dvci, NULL, &t->rt_depth_view) != VK_SUCCESS) {
        RHI_LOG("rt depth vkCreateImageView failed\n"); goto fail;
    }
    t->depth_readable = depth_readable;
    t->rt_pass = depth_sampleable ? dev->rt_ds_rp : dev->rt_rp;
    VkImageView fb_atts[2] = { t->view, t->rt_depth_view };
    VkFramebufferCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = t->rt_pass,
        .attachmentCount = 2, .pAttachments = fb_atts,
        .width = width, .height = height, .layers = 1,
    };
    if (vkCreateFramebuffer(dev->device, &fci, NULL, &t->rt_fb) != VK_SUCCESS) {
        RHI_LOG("rt vkCreateFramebuffer failed\n"); goto fail;
    }
    /* Depth-sampled RT: a DEPTH-aspect-only view (a sampler view must select
     * exactly one aspect of a combined depth+stencil image) + a descriptor set
     * so the pre-pass depth binds like any texture (texelFetch .r = depth). */
    if (depth_sampleable) {
        VkImageViewCreateInfo svci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->rt_depth_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = dev->depth_format,
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                  .levelCount = 1, .layerCount = 1 },
        };
        if (vkCreateImageView(dev->device, &svci, NULL, &t->rt_depth_sample_view) != VK_SUCCESS) {
            RHI_LOG("rt depth sample vkCreateImageView failed\n"); goto fail;
        }
        VkDescriptorSetAllocateInfo ddsi = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = dev->desc_pool, .descriptorSetCount = 1,
            .pSetLayouts = &dev->vram_set_layout,
        };
        if (vkAllocateDescriptorSets(dev->device, &ddsi, &t->depth_set) != VK_SUCCESS) {
            RHI_LOG("rt depth vkAllocateDescriptorSets failed\n"); goto fail;
        }
        VkDescriptorImageInfo ddii = {
            .sampler = dev->vram_sampler, .imageView = t->rt_depth_sample_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet dw = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = t->depth_set,
            .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ddii,
        };
        vkUpdateDescriptorSets(dev->device, 1, &dw, 0, NULL);
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
    if (depth_sampleable) {
        /* Same for the sampleable DEPTH image (both aspects — a combined
         * depth+stencil image transitions as a whole without the
         * separateDepthStencilLayouts feature). */
        VkImageMemoryBarrier db = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = t->rt_depth_image,
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT |
                                                VK_IMAGE_ASPECT_STENCIL_BIT,
                                  .levelCount = 1, .layerCount = 1 },
        };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, NULL, 0, NULL, 1, &db);
    }
    one_shot_end(dev, cb);
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return t;

fail:
    rhi_destroy_texture(dev, t);
    return NULL;
}

rhi_texture *rhi_create_render_target(rhi_device *dev, uint32_t width, uint32_t height)
{
    return create_render_target_impl(dev, width, height, false, false);
}

rhi_texture *rhi_create_render_target_depth_sampled(rhi_device *dev,
                                                    uint32_t width, uint32_t height)
{
    return create_render_target_impl(dev, width, height, true, false);
}

rhi_texture *rhi_create_render_target_depth_readable(rhi_device *dev,
                                                     uint32_t width, uint32_t height)
{
    return create_render_target_impl(dev, width, height, true, true);
}

void rhi_bind_depth_sampler(rhi_device *dev, rhi_texture *rt, uint32_t set)
{
    if (!dev->frame_active || !rt || !rt->depth_set) return;
    vkCmdBindDescriptorSets(dev->cmd[dev->frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                            dev->cur_layout, set, 1, &rt->depth_set, 0, NULL);
}

void rhi_set_viewport(rhi_device *dev, int w, int h)
{
    if (!dev->frame_active) return;
    VkViewport vp = { .x = 0.0f, .y = (float)h, .width = (float)w, .height = -(float)h,
                      .minDepth = 0.0f, .maxDepth = 1.0f };
    VkRect2D sc = { .offset = {0, 0}, .extent = { (uint32_t)w, (uint32_t)h } };
    vkCmdSetViewport(dev->cmd[dev->frame], 0, 1, &vp);
    vkCmdSetScissor(dev->cmd[dev->frame], 0, 1, &sc);
}

void rhi_swapchain_extent(const rhi_device *dev, int *w, int *h)
{
    if (w) *w = dev ? (int)dev->sc_extent.width  : 0;
    if (h) *h = dev ? (int)dev->sc_extent.height : 0;
}

void rhi_pass_extent(const rhi_device *dev, int *w, int *h)
{
    if (w) *w = dev ? (int)dev->cur_extent.width  : 0;
    if (h) *h = dev ? (int)dev->cur_extent.height : 0;
}

void rhi_begin_render_target(rhi_device *dev, rhi_texture *rt,
                             float r, float g, float b, float a)
{
    if (!dev->frame_active || !rt || !rt->is_rt) return;
    VkClearValue cv[2];
    cv[0].color.float32[0] = r; cv[0].color.float32[1] = g;
    cv[0].color.float32[2] = b; cv[0].color.float32[3] = a;
    cv[1].depthStencil.depth = dev->depth_clear[rt->depth_class]; cv[1].depthStencil.stencil = 0;
    VkExtent2D ext = { rt->width, rt->height };
    VkRenderPassBeginInfo rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = rt->rt_pass ? rt->rt_pass : dev->rt_rp, .framebuffer = rt->rt_fb,
        .renderArea = { .offset = {0, 0}, .extent = ext },
        .clearValueCount = 2, .pClearValues = cv,
    };
    vkCmdBeginRenderPass(dev->cmd[dev->frame], &rp, VK_SUBPASS_CONTENTS_INLINE);
    dev->cur_extent = ext;   /* render area of this (render-target) pass */
    dev->cur_depth_clear = cv[1].depthStencil.depth;
    /* Y-flipped viewport, matching rhi_begin_offscreen. */
    VkViewport vp = { .x = 0.0f, .y = (float)rt->height,
                      .width = (float)rt->width, .height = -(float)rt->height,
                      .minDepth = 0.0f, .maxDepth = 1.0f };
    VkRect2D scissor = { .offset = {0, 0}, .extent = ext };
    vkCmdSetViewport(dev->cmd[dev->frame], 0, 1, &vp);
    vkCmdSetScissor(dev->cmd[dev->frame], 0, 1, &scissor);
    /* Belt-only: a defined value if a depth_bias pipeline is somehow bound
     * first in the pass. NOT a guarantee — binding any static-state pipeline
     * invalidates dynamic depth bias, so callers must set it after every
     * depth_bias pipeline bind (see rhi.h). */
    vkCmdSetDepthBias(dev->cmd[dev->frame], 0.0f, 0.0f, 0.0f);
}

void rhi_end_render_target(rhi_device *dev, rhi_texture *rt)
{
    if (!dev->frame_active || !rt || !rt->is_rt) return;
    vkCmdEndRenderPass(dev->cmd[dev->frame]);
    rt->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; /* rt_rp finalLayout */
}

void rhi_set_render_target_depth_class(rhi_texture *rt, rhi_depth_class cls)
{
    if (rt && rt->is_rt) rt->depth_class = cls;
}

/* Clear the depth attachment to far (the pass's depth class's clear value)
 * WITHIN the current render pass — used
 * to isolate a late depth-tested overlay (the 3D HUD icons) from world depth so
 * it sorts internally yet always draws on top. vkCmdClearAttachments clears in
 * the active subpass over the given rect (no new render pass). */
void rhi_clear_depth(rhi_device *dev)
{
    if (!dev->frame_active) return;
    VkClearAttachment att = {
        .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
        .clearValue = { .depthStencil = { .depth = dev->cur_depth_clear, .stencil = 0 } },
    };
    VkClearRect rect = {
        .rect = { .offset = {0, 0}, .extent = dev->cur_extent },
        .baseArrayLayer = 0, .layerCount = 1,
    };
    vkCmdClearAttachments(dev->cmd[dev->frame], 1, &att, 1, &rect);
}

/* Clear the STENCIL aspect to 0 within the current pass — between per-object
 * stencil-mask groups (fixed reference 1 + clear = per-object isolation). */
void rhi_clear_stencil(rhi_device *dev)
{
    if (!dev->frame_active) return;
    VkClearAttachment att = {
        .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
        .clearValue = { .depthStencil = { .depth = 1.0f, .stencil = 0 } },
    };
    VkClearRect rect = {
        .rect = { .offset = {0, 0}, .extent = dev->cur_extent },
        .baseArrayLayer = 0, .layerCount = 1,
    };
    vkCmdClearAttachments(dev->cmd[dev->frame], 1, &att, 1, &rect);
}

struct rhi_stage_chunk {
    rhi_buffer            *buf;
    size_t                 cap, used;
    struct rhi_stage_chunk *next;
};

/* A `need`-byte range of host-visible staging for an in-frame upload to
 * `tex`, disjoint from every range handed out for this texture in the
 * current frame. Offsets are 4-byte aligned (VkBufferImageCopy.bufferOffset
 * for 2- and 4-byte texels). Returns false only when a chunk could not be
 * allocated. */
static bool stage_acquire(rhi_device *dev, rhi_texture *tex, size_t need,
                          VkBuffer *out_buf, uint8_t **out_map, size_t *out_off)
{
    const uint32_t slot = dev->frame;
    if (tex->stage_serial[slot] != s_frame_serial) {
        for (struct rhi_stage_chunk *c = tex->stage_arena[slot]; c; c = c->next) c->used = 0;
        tex->stage_serial[slot] = s_frame_serial;
    }
    struct rhi_stage_chunk *c = tex->stage_arena[slot], *last = NULL;
    for (; c; last = c, c = c->next) {
        size_t off = (c->used + 3) & ~(size_t)3;
        if (off + need <= c->cap) {
            *out_buf = c->buf->buffer; *out_map = (uint8_t *)c->buf->mapped + off; *out_off = off;
            c->used = off + need;
            return true;
        }
    }
    size_t cap = (size_t)tex->width * tex->height * tex->bpp;
    if (cap < need) cap = need;
    struct rhi_stage_chunk *n = calloc(1, sizeof(*n));
    if (!n) return false;
    n->buf = create_host_buffer(dev, cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, /*prefer_cached=*/false);
    if (!n->buf || !n->buf->mapped) { if (n->buf) rhi_destroy_buffer(dev, n->buf); free(n); return false; }
    n->cap = cap; n->used = need;
    if (last) last->next = n; else tex->stage_arena[slot] = n;
    *out_buf = n->buf->buffer; *out_map = (uint8_t *)n->buf->mapped; *out_off = 0;
    return true;
}

static void update_image(rhi_device *dev, rhi_texture *tex, const void *pixels)
{
    if (!tex || !tex->staging) return;
    const size_t bytes = (size_t)tex->width * tex->height * tex->bpp;

    /* In-frame path (W2.2): a frame is being recorded, so append the upload
     * to ITS command buffer instead of a blocking one-shot submit — the old
     * per-present `vkQueueWaitIdle` drained the whole GPU every present and
     * defeated the frames-in-flight design. MUST be called before the
     * frame's first render pass (callers upload right after
     * rhi_begin_frame). Staging is a per-frame ring slot: the previous
     * frame's transfer may still be reading its own slot, but THIS frame's
     * slot was fenced by rhi_begin_frame. Barriers are recorded per frame in
     * submission order, so layout tracking stays a plain CPU variable. */
    VkBuffer src;
    VkCommandBuffer cb;
    size_t src_off = 0;
    bool one_shot = !dev->frame_active;
    if (!pixels) { RHI_LOG("update_image: NULL pixels (tex %ux%u)\n", tex->width, tex->height); return; }
    if (one_shot) {
        if (!tex->staging->mapped) { RHI_LOG("update_image: staging unmapped\n"); return; }
        memcpy(tex->staging->mapped, pixels, bytes);
        src = tex->staging->buffer;
        cb  = one_shot_begin(dev);
    } else {
        uint8_t *map;
        if (!stage_acquire(dev, tex, bytes, &src, &map, &src_off)) return;
        memcpy(map, pixels, bytes);
        cb  = dev->cmd[dev->frame];
    }

    image_barrier(cb, tex->image, tex->layout,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = {
        .bufferOffset = src_off,
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .imageExtent = { tex->width, tex->height, 1 },
    };
    vkCmdCopyBufferToImage(cb, src, tex->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    image_barrier(cb, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (one_shot) one_shot_end(dev, cb);
    tex->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void rhi_update_vram(rhi_device *dev, rhi_texture *tex, const uint16_t *pixels)
{
    update_image(dev, tex, pixels);
}

/* W2.4 dirty-rect sub-upload: N pre-clamped rects from the full-stride CPU
 * mirror, packed sequentially into the staging slot, one barrier pair around
 * one N-region vkCmdCopyBufferToImage. Falls back to a full upload when the
 * packed payload wouldn't fit the (full-image-sized) staging slot. */
#define RHI_VRAM_MAX_RECTS 96
/* Shared body: `pack` fills the staging buffer with the rects' texels in
 * upload order (each rect's rows contiguous, rects at 4-byte-aligned
 * offsets); the regions describe that layout. */
static void update_vram_rects_impl(rhi_device *dev, rhi_texture *tex,
                                   const uint16_t *mirror, const uint8_t *packed,
                                   const rhi_rect *rects, int nrects)
{
    if (!tex || !tex->staging || nrects <= 0) return;
    /* Whole-image fallbacks need a mirror; a packed source has none and is
     * batched to fit by its caller, so an oversize packed batch is dropped. */
    if (nrects > RHI_VRAM_MAX_RECTS) { if (mirror) update_image(dev, tex, mirror); return; }
    const size_t cap = (size_t)tex->width * tex->height * tex->bpp;
    size_t need = 0;
    for (int i = 0; i < nrects; i++) {
        if (rects[i].w <= 0 || rects[i].h <= 0) continue;
        need = (need + 3) & ~(size_t)3;
        need += (size_t)rects[i].w * rects[i].h * tex->bpp;
    }
    if (need == 0) return;
    if (need > cap) { if (mirror) update_image(dev, tex, mirror); return; }

    VkBuffer src;
    VkCommandBuffer cb;
    uint8_t *map;
    size_t src_off = 0;
    bool one_shot = !dev->frame_active;
    if (one_shot) {
        map = (uint8_t *)tex->staging->mapped;
        src = tex->staging->buffer;
        cb  = one_shot_begin(dev);
    } else {
        if (!stage_acquire(dev, tex, need, &src, &map, &src_off)) return;
        cb  = dev->cmd[dev->frame];
    }

    image_barrier(cb, tex->image, tex->layout,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy regions[RHI_VRAM_MAX_RECTS];
    uint32_t nregions = 0;
    size_t off = 0;
    for (int i = 0; i < nrects; i++) {
        const int rx = rects[i].x, ry = rects[i].y, rw = rects[i].w, rh = rects[i].h;
        if (rw <= 0 || rh <= 0) continue;
        off = (off + 3) & ~(size_t)3;               /* bufferOffset: 4-byte aligned */
        uint8_t *dst = map + off;
        const size_t row = (size_t)rw * tex->bpp;
        if (packed) memcpy(dst, packed + off, row * (size_t)rh);
        else
            for (int r = 0; r < rh; r++)
                memcpy(dst + (size_t)r * row,
                       (const uint8_t *)mirror + (((size_t)(ry + r) * tex->width + rx) * tex->bpp),
                       row);
        regions[nregions] = (VkBufferImageCopy){
            .bufferOffset = src_off + off,
            .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
            .imageOffset = { rx, ry, 0 },
            .imageExtent = { (uint32_t)rw, (uint32_t)rh, 1 },
        };
        nregions++;
        off += row * rh;
    }
    vkCmdCopyBufferToImage(cb, src, tex->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nregions, regions);
    image_barrier(cb, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (one_shot) one_shot_end(dev, cb);
    tex->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void rhi_update_vram_rects(rhi_device *dev, rhi_texture *tex,
                           const uint16_t *mirror,
                           const rhi_rect *rects, int nrects)
{
    update_vram_rects_impl(dev, tex, mirror, NULL, rects, nrects);
}

void rhi_update_vram_rects_packed(rhi_device *dev, rhi_texture *tex,
                                  const uint8_t *packed, size_t packed_bytes,
                                  const rhi_rect *rects, int nrects)
{
    (void)packed_bytes;
    if (!tex) return;
    /* The shared body falls back to a whole-image upload past its region cap
     * or when the payload outgrows the (image-sized) staging slot, and that
     * fallback needs a mirror a packed source does not have. Split the batch
     * on both bounds instead: overlapping dirty rects (a level load rewrites
     * the same texture pages many times before one present drains them) pack
     * to more bytes than the image holds. */
    const size_t cap = (size_t)tex->width * tex->height * tex->bpp;
    while (nrects > 0) {
        int n = 0; size_t off = 0;
        while (n < nrects && n < RHI_VRAM_MAX_RECTS) {
            const size_t next = ((off + 3) & ~(size_t)3) +
                                (size_t)rects[n].w * rects[n].h * tex->bpp;
            if (n > 0 && next > cap) break;
            off = next; n++;
        }
        update_vram_rects_impl(dev, tex, NULL, packed, rects, n);
        off = (off + 3) & ~(size_t)3;
        packed += off; rects += n; nrects -= n;
    }
}


void rhi_update_rgba8(rhi_device *dev, rhi_texture *tex, const void *pixels)
{
    update_image(dev, tex, pixels);
}

/* Sub-rect uploads from TIGHT per-rect source buffers (each rects[i].pixels
 * is w*h*4 bytes) — the atlas-append path. Same staging/in-frame semantics
 * as update_image; payloads are packed into one staging slot and copied in
 * one barrier pair. Rects that don't fit the remaining slot are dropped
 * (the caller re-queues them next frame). */
void rhi_update_rgba8_rects(rhi_device *dev, rhi_texture *tex,
                            const rhi_tex_rect_src *rects, int nrects)
{
    if (!tex || !tex->staging || nrects <= 0) return;
    const size_t cap = (size_t)tex->width * tex->height * tex->bpp;

    VkBuffer src;
    VkCommandBuffer cb;
    uint8_t *dst;
    size_t src_off = 0;
    /* The batch this call uploads: rects in order until the next one would
     * overflow the image-sized bound (the caller re-queues the rest). */
    size_t need = 0; int nfit = 0, seen = 0;
    for (int i = 0; i < nrects && seen < 96; i++) {
        const rhi_tex_rect_src *r = &rects[i];
        if (!r->pixels || r->w <= 0 || r->h <= 0) continue;
        size_t bytes = (size_t)r->w * r->h * 4u;
        size_t at = (need + 3) & ~(size_t)3;
        if (at + bytes > cap) break;
        need = at + bytes; nfit = i + 1; seen++;
    }
    if (need == 0) return;
    bool one_shot = !dev->frame_active;
    if (one_shot) {
        dst = (uint8_t *)tex->staging->mapped;
        src = tex->staging->buffer;
        cb  = one_shot_begin(dev);
    } else {
        if (!stage_acquire(dev, tex, need, &src, &dst, &src_off)) return;
        cb  = dev->cmd[dev->frame];
    }

    VkBufferImageCopy regions[96];
    int nreg = 0;
    size_t off = 0;
    for (int i = 0; i < nfit && nreg < 96; i++) {
        const rhi_tex_rect_src *r = &rects[i];
        if (!r->pixels || r->w <= 0 || r->h <= 0) continue;
        size_t bytes = (size_t)r->w * r->h * 4u;
        off = (off + 3) & ~(size_t)3;
        memcpy(dst + off, r->pixels, bytes);
        regions[nreg++] = (VkBufferImageCopy){
            .bufferOffset = src_off + off,
            .bufferRowLength = (uint32_t)r->w,
            .bufferImageHeight = (uint32_t)r->h,
            .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
            .imageOffset = { r->x, r->y, 0 },
            .imageExtent = { (uint32_t)r->w, (uint32_t)r->h, 1 },
        };
        off += bytes;
    }
    if (nreg == 0) { if (one_shot) one_shot_end(dev, cb); return; }

    image_barrier(cb, tex->image, tex->layout,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(cb, src, tex->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           (uint32_t)nreg, regions);
    image_barrier(cb, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (one_shot) one_shot_end(dev, cb);
    tex->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
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

bool rhi_cycle_rt_readback(rhi_device *dev, rhi_texture *rt,
                           void *out, size_t out_bytes, int *out_w, int *out_h)
{
    if (!dev || !dev->frame_active || !rt || !rt->is_rt) return false;
    uint32_t fi = dev->frame;

    /* (1) Retrieve the capture queued the last time this slot cycled (its copy
     * rode cmd[fi] under in_flight[fi], which begin_frame already waited on). */
    bool got = false;
    if (dev->grid_ring[fi].pending && dev->grid_ring[fi].staging &&
        dev->grid_ring[fi].w > 0 && dev->grid_ring[fi].h > 0) {
        int pw = dev->grid_ring[fi].w, ph = dev->grid_ring[fi].h;
        size_t need = (size_t)pw * (size_t)ph * 4u;
        if (out && out_bytes >= need) {
            /* Normalize to R,G,B,A byte order (the RT color format may be BGRA). */
            readback_swizzle(dev, dev->grid_ring[fi].staging->mapped, out, pw, ph);
            if (out_w) *out_w = pw;
            if (out_h) *out_h = ph;
            got = true;
        }
        dev->grid_ring[fi].pending = false;
    }

    /* (2) Queue THIS frame's copy. The RT sits in SHADER_READ_ONLY after its
     * render pass (rt_rp finalLayout); round-trip it through TRANSFER_SRC in
     * this frame's command buffer. Call AFTER the pass that renders the RT. */
    int w = (int)rt->width, h = (int)rt->height;
    if (w > 0 && h > 0) {
        size_t need = (size_t)w * (size_t)h * 4u;
        rhi_buffer **staging = &dev->grid_ring[fi].staging;
        if (!*staging || (*staging)->size < need) {
            if (*staging) rhi_destroy_buffer(dev, *staging);
            *staging = create_host_buffer(dev, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          /*prefer_cached=*/true);
        }
        if (*staging) {
            VkImageMemoryBarrier to_src = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = rt->image,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            };
            vkCmdPipelineBarrier(dev->cmd[fi],
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, NULL, 0, NULL, 1, &to_src);
            VkBufferImageCopy region = {
                .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
                .imageExtent = { (uint32_t)w, (uint32_t)h, 1 },
            };
            vkCmdCopyImageToBuffer(dev->cmd[fi], rt->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   (*staging)->buffer, 1, &region);
            VkImageMemoryBarrier back = to_src;
            back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            back.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            back.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(dev->cmd[fi],
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, NULL, 0, NULL, 1, &back);
            dev->grid_ring[fi].pending = true;
            dev->grid_ring[fi].w = w;
            dev->grid_ring[fi].h = h;
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

bool rhi_read_render_target_depth(rhi_device *dev, rhi_texture *rt, float *out,
                                  size_t out_count, int *out_w, int *out_h)
{
    if (!rt || !rt->is_rt || !rt->depth_readable || !rt->rt_depth_image) return false;
    int w = (int)rt->width, h = (int)rt->height;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    size_t n = (size_t)w * (size_t)h;
    if (!out || out_count < n || w <= 0 || h <= 0) return false;

    /* The depth image is left in SHADER_READ_ONLY by the depth-sampled pass
     * (both aspects: a combined image transitions as a whole). The depth
     * aspect copies out as 4 bytes per texel on both formats: a float on
     * D32_SFLOAT_S8_UINT, 24 unorm bits in the low bytes on
     * D24_UNORM_S8_UINT. */
    vkDeviceWaitIdle(dev->device);
    rhi_buffer *rb = create_host_buffer(dev, n * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        /*prefer_cached=*/true);
    if (!rb) return false;
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = rt->rt_depth_image,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT |
                                            VK_IMAGE_ASPECT_STENCIL_BIT,
                              .levelCount = 1, .layerCount = 1 },
    };
    VkCommandBuffer cb = one_shot_begin(dev);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy region = {
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .layerCount = 1 },
        .imageExtent = { (uint32_t)w, (uint32_t)h, 1 },
    };
    vkCmdCopyImageToBuffer(cb, rt->rt_depth_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           rb->buffer, 1, &region);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    one_shot_end(dev, cb);
    const uint8_t *src = (const uint8_t *)rb->mapped;
    const bool f32 = dev->depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    for (size_t i = 0; i < n; i++) {
        uint32_t u;
        memcpy(&u, src + i * 4u, 4);
        if (f32) memcpy(&out[i], &u, 4);
        else     out[i] = (float)(u & 0xFFFFFFu) / 16777215.0f;
    }
    rhi_destroy_buffer(dev, rb);
    return true;
}

void rhi_destroy_texture(rhi_device *dev, rhi_texture *tex)
{
    if (!tex) return;
    if (tex->set)       vkFreeDescriptorSets(dev->device, dev->desc_pool, 1, &tex->set);
    if (tex->depth_set) vkFreeDescriptorSets(dev->device, dev->desc_pool, 1, &tex->depth_set);
    if (tex->rt_depth_sample_view) vkDestroyImageView(dev->device, tex->rt_depth_sample_view, NULL);
    if (tex->staging) rhi_destroy_buffer(dev, tex->staging);
    for (int i = 0; i < RHI_FRAMES_IN_FLIGHT; i++) {
        struct rhi_stage_chunk *c = tex->stage_arena[i];
        while (c) { struct rhi_stage_chunk *n = c->next; rhi_destroy_buffer(dev, c->buf); free(c); c = n; }
        tex->stage_arena[i] = NULL;
    }
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

/* Shared pipeline builder: both public entry points store their shader
 * sources on the rhi_pipeline and land in pipeline_compile, which (re)creates
 * the Vulkan objects from those stored sources — the same path
 * rhi_rebuild_pipelines uses to re-bake live pipelines after a spec-constant
 * change. desc->postfx selects the mod-postfx layout — no vertex input
 * (fullscreen triangle from gl_VertexIndex), sets 0..2 = one sampler
 * each, set 3 = the dynamic-UBO pair. */
/* Specialization constant 0 (`DEPTH_REVERSED`): the pipeline's depth class's
 * convention on this device (depth_convention_init): true = reversed
 * z_ndc = 1/vz, false = forward z_ndc = 1 - 1/vz (psx_viewz.glsl
 * op_scene_clip). Declared by the stages that emit scene depth
 * (psx_scene.vert, quad.vert, terrain.vert, shadow_decal_tex.vert) and by
 * every stage that decodes gl_FragCoord.z back to view-Z (hd_tex_cutout.frag,
 * quad_key.frag, psx_scene_key.frag, terrain_shade.glsl, shadow_decal.frag,
 * shadow_decal_tex.frag, openpete_psx_material.glsl, openpete_psx_decal.glsl)
 * — the decode must follow the encode, which is why the entry is attached to
 * BOTH stages below. A stage that declares no spec constant ignores it. */
bool rhi_depth_reversed(const rhi_device *dev)
{
    return dev && dev->depth_rev[RHI_DEPTH_SCENE];
}

/* Vertex-stage specialization constant 1 (`AFFINE_UV`): PSX-affine UV/colour
 * interpolation (--texture-mapping affine). Like DEPTH_AFFINE it is captured
 * at pipeline creation — set it before the pipelines are built, or follow a
 * live change with rhi_rebuild_pipelines to re-bake the existing ones.
 * Declared by psx_scene.vert, quad.vert, terrain.vert, terrain_key.vert and
 * moby_rank.vert: the key and rank stages have to select the same texel
 * lookups the scene draw samples, or a raster stamps where the face's own
 * paint discards. */
static VkBool32 s_vs_affine_uv = VK_FALSE;

int rhi_depth_format(const rhi_device *dev) { return dev ? (int)dev->depth_format : 0; }

unsigned rhi_depth_pipelines_by_compare(const rhi_device *dev, int compare_op)
{
    return (dev && compare_op >= 0 && compare_op < 8) ? dev->pipes_by_cmp[compare_op] : 0u;
}

void rhi_depth_report(const rhi_device *dev)
{
    if (!dev) return;
    RHI_LOG("depth: format %s, scene %s compare %d clear %g, forward compare %d clear %g; "
            "pipelines LESS_OR_EQUAL=%u GREATER_OR_EQUAL=%u ALWAYS=%u\n",
            depth_format_name(dev->depth_format),
            dev->depth_rev[RHI_DEPTH_SCENE] ? "reversed 1/vz" : "forward 1-1/vz",
            (int)dev->depth_cmp[RHI_DEPTH_SCENE], (double)dev->depth_clear[RHI_DEPTH_SCENE],
            (int)dev->depth_cmp[RHI_DEPTH_FORWARD], (double)dev->depth_clear[RHI_DEPTH_FORWARD],
            dev->pipes_by_cmp[VK_COMPARE_OP_LESS_OR_EQUAL],
            dev->pipes_by_cmp[VK_COMPARE_OP_GREATER_OR_EQUAL],
            dev->pipes_by_cmp[VK_COMPARE_OP_ALWAYS]);
}
void rhi_set_affine_uv(int on) { s_vs_affine_uv = on ? VK_TRUE : VK_FALSE; }

/* Fragment-stage specialization constant 2 (`NOTEX`): untextured lighting
 * view — the textured scene fragments replace the sampled texel colour with
 * the modulation-neutral grey (128/255), so the output is the raw gouraud
 * vertex colour while transparency cutouts keep their real texel shapes.
 * Debug view; live toggle via rhi_rebuild_pipelines like the others. */
static VkBool32 s_fs_notex = VK_FALSE;
void rhi_set_notex(int on) { s_fs_notex = on ? VK_TRUE : VK_FALSE; }

/* Wireframe view (rhi.h rhi_set_wireframe): the scene pipelines rasterize
 * as lines and bake NOTEX on. Captured at pipeline creation like the
 * specialization constants, so a live change rebuilds every pipeline in
 * place (rhi_rebuild_pipelines); there is no second pipeline set. */
static bool s_wireframe = false;
void rhi_set_wireframe(int on) { s_wireframe = on != 0; }

/* A scene draw under the wireframe view: depth-tested, or marked
 * wire_scene; a colour-write-off pass that keeps the stored depth (the
 * pre-pass into the sampled opaque depth, the portal aperture mask) stays
 * filled so its coverage stays whole-face. */
static bool pipeline_wire(const rhi_pipeline_desc *desc)
{
    if (!s_wireframe || desc->postfx) return false;
    if (desc->wire_scene) return true;
    return desc->depth_test && (!desc->color_write_off || desc->depth_overwrite);
}

static VkPolygonMode pipeline_polygon_mode(const rhi_device *dev,
                                           const rhi_pipeline_desc *desc)
{
    return (pipeline_wire(desc) && dev->has_fill_non_solid)
               ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
}

/* Spec constant 2 per pipeline: the global NOTEX view, or the wireframe
 * view's lighting colour on a wired pipeline. */
static VkBool32 pipeline_notex(const rhi_pipeline_desc *desc)
{
    return (s_fs_notex || pipeline_wire(desc)) ? VK_TRUE : VK_FALSE;
}

/* Fragment-stage specialization constant 3 (`MOBY_TRUESCOPE`): scopes the
 * key-contest's moby og-key yields by the covering fragment's own TRUE
 * view-Z. Default 1 — og's pull+push OT compounding reads as
 * moby-through-terrain punch-through from ordinary gameplay angles too;
 * 0 restores og's painter for A/B. Live toggle via rhi_rebuild_pipelines
 * like the others. */
static VkBool32 s_fs_moby_truescope = VK_TRUE;
void rhi_set_moby_truescope(int on) { s_fs_moby_truescope = on ? VK_TRUE : VK_FALSE; }

/* Fragment-stage specialization constant 4 (`SLOT_CONTEST`): the key
 * contest compares 128-world ordering-table slot indices instead of world
 * distances and resolves an equal slot to the terrain. A per-game constant
 * (game_layout.terrain.slot_ot_contest), set before the pipelines are built
 * and never toggled after. */
static VkBool32 s_fs_slot_contest = VK_FALSE;
void rhi_set_slot_contest(int on) { s_fs_slot_contest = on ? VK_TRUE : VK_FALSE; }

/* Terrain sector-mesh layout (rhi_terrain_vertex, 64 B): integer-packed
 * retained world-space vertices; see rhi.h for the field/bit contract.
 * SINT/UINT formats — terrain.vert consumes ivec/uvec, no normalization. */
#define TERRAIN_VERTEX_ATTRS \
    { .location = 0, .binding = 0, .format = VK_FORMAT_R16G16B16A16_SINT, .offset = offsetof(rhi_terrain_vertex, px) }, \
    { .location = 1, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UINT,     .offset = offsetof(rhi_terrain_vertex, uvf_u) }, \
    { .location = 2, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UNORM,    .offset = offsetof(rhi_terrain_vertex, rgba_far) }, \
    { .location = 3, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UNORM,    .offset = offsetof(rhi_terrain_vertex, rgba_near) }, \
    { .location = 4, .binding = 0, .format = VK_FORMAT_R32G32B32A32_UINT, .offset = offsetof(rhi_terrain_vertex, mat_far0) }, \
    { .location = 5, .binding = 0, .format = VK_FORMAT_R32_UINT,          .offset = offsetof(rhi_terrain_vertex, face_flags) }, \
    { .location = 6, .binding = 0, .format = VK_FORMAT_R16G16B16A16_SINT, .offset = offsetof(rhi_terrain_vertex, c0x) }, \
    { .location = 7, .binding = 0, .format = VK_FORMAT_R16G16B16A16_SINT, .offset = offsetof(rhi_terrain_vertex, c1y) }, \
    { .location = 8, .binding = 0, .format = VK_FORMAT_R16G16B16A16_SINT, .offset = offsetof(rhi_terrain_vertex, c2z) }
static const VkVertexInputAttributeDescription k_terrain_attrs[9] = { TERRAIN_VERTEX_ATTRS };
/* terrain_anim: cur verts at binding 0 + the prev-tick twin at binding 1
 * (a parallel array, same vertex indexing — see rhi_terrain_prev). */
static const VkVertexInputBindingDescription k_terrain_anim_binds[2] = {
    { .binding = 0, .stride = sizeof(rhi_terrain_vertex),
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
    { .binding = 1, .stride = sizeof(rhi_terrain_prev),
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
};
static const VkVertexInputAttributeDescription k_terrain_anim_attrs[14] = {
    TERRAIN_VERTEX_ATTRS,
    { .location = 9,  .binding = 1, .format = VK_FORMAT_R16G16B16A16_SINT, .offset = offsetof(rhi_terrain_prev, px) },
    { .location = 10, .binding = 1, .format = VK_FORMAT_R16G16B16A16_SINT, .offset = offsetof(rhi_terrain_prev, c0x) },
    { .location = 11, .binding = 1, .format = VK_FORMAT_R16G16B16A16_SINT, .offset = offsetof(rhi_terrain_prev, c1y) },
    { .location = 12, .binding = 1, .format = VK_FORMAT_R16G16B16A16_SINT, .offset = offsetof(rhi_terrain_prev, c2z) },
    { .location = 13, .binding = 1, .format = VK_FORMAT_R32G32_UINT,       .offset = offsetof(rhi_terrain_prev, rgb_far) },
};
/* terrain_near: cur verts at binding 0 + the emitted-tri member context
 * at binding 1 (a parallel array — see rhi_terrain_nearctx). */
static const VkVertexInputBindingDescription k_terrain_near_binds[2] = {
    { .binding = 0, .stride = sizeof(rhi_terrain_vertex),
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
    { .binding = 1, .stride = sizeof(rhi_terrain_nearctx),
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
};
static const VkVertexInputAttributeDescription k_terrain_near_attrs[13] = {
    TERRAIN_VERTEX_ATTRS,
    { .location = 9,  .binding = 1, .format = VK_FORMAT_R8G8B8A8_UINT, .offset = offsetof(rhi_terrain_nearctx, a_uf) },
    { .location = 10, .binding = 1, .format = VK_FORMAT_R32G32_UINT,   .offset = offsetof(rhi_terrain_nearctx, a_far) },
    { .location = 11, .binding = 1, .format = VK_FORMAT_R8G8B8A8_UINT, .offset = offsetof(rhi_terrain_nearctx, b_uf) },
    { .location = 12, .binding = 1, .format = VK_FORMAT_R32G32_UINT,   .offset = offsetof(rhi_terrain_nearctx, b_far) },
};
#undef TERRAIN_VERTEX_ATTRS
/* terrain_cells: one INSTANCE-rate binding of cell records (rhi_terrain_cell;
 * see rhi.h), no per-vertex binding — the vertex stage derives every cell
 * vertex from its record and gl_VertexIndex. Locations 0-2 carry the corners
 * in rhi_terrain_vertex's c0x..c3z packing, 3 the face flags + record info,
 * 4 the near colours, 5-12 the eight cell tiles; the anim records append the
 * previous corners at 13-15. */
#define CELL_TILE_ATTR(i) \
    { .location = 5 + (i), .binding = 0, .format = VK_FORMAT_R32G32B32A32_UINT, \
      .offset = offsetof(rhi_terrain_cell, tile) + (i) * sizeof(rhi_terrain_cell_tile) }
static const VkVertexInputAttributeDescription k_terrain_cell_attrs[16] = {
    { .location = 0, .binding = 0, .format = VK_FORMAT_R16G16B16A16_SINT, .offset = offsetof(rhi_terrain_cell, c0x) },
    { .location = 1, .binding = 0, .format = VK_FORMAT_R16G16B16A16_SINT, .offset = offsetof(rhi_terrain_cell, c1y) },
    { .location = 2, .binding = 0, .format = VK_FORMAT_R16G16B16A16_SINT, .offset = offsetof(rhi_terrain_cell, c2z) },
    { .location = 3, .binding = 0, .format = VK_FORMAT_R32G32_UINT,       .offset = offsetof(rhi_terrain_cell, face_flags) },
    { .location = 4, .binding = 0, .format = VK_FORMAT_R32G32B32A32_UINT, .offset = offsetof(rhi_terrain_cell, near_rgb) },
    CELL_TILE_ATTR(0), CELL_TILE_ATTR(1), CELL_TILE_ATTR(2), CELL_TILE_ATTR(3),
    CELL_TILE_ATTR(4), CELL_TILE_ATTR(5), CELL_TILE_ATTR(6), CELL_TILE_ATTR(7),
    { .location = 13, .binding = 0, .format = VK_FORMAT_R16G16B16A16_SINT, .offset = offsetof(rhi_terrain_cell_anim, p0x) },
    { .location = 14, .binding = 0, .format = VK_FORMAT_R16G16B16A16_SINT, .offset = offsetof(rhi_terrain_cell_anim, p1y) },
    { .location = 15, .binding = 0, .format = VK_FORMAT_R16G16B16A16_SINT, .offset = offsetof(rhi_terrain_cell_anim, p2z) },
};
#undef CELL_TILE_ATTR
static const VkVertexInputBindingDescription k_terrain_cell_bind = {
    .binding = 0, .stride = sizeof(rhi_terrain_cell), .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE,
};
static const VkVertexInputBindingDescription k_terrain_cell_anim_bind = {
    .binding = 0, .stride = sizeof(rhi_terrain_cell_anim), .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE,
};

/* The terrain pipelines' vertex input: the plain sector mesh, its anim or
 * near twin pair, or a cell-record flavour. `vi` arrives holding the
 * one-binding default, whose binding stride the plain mesh overrides. */
static void terrain_vertex_input(const rhi_pipeline_desc *desc,
                                 VkPipelineVertexInputStateCreateInfo *vi)
{
    static const VkVertexInputBindingDescription plain = {
        .binding = 0, .stride = sizeof(rhi_terrain_vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    vi->vertexBindingDescriptionCount = 1;
    vi->pVertexBindingDescriptions = &plain;
    vi->vertexAttributeDescriptionCount = 9;
    vi->pVertexAttributeDescriptions = k_terrain_attrs;
    if (desc->terrain_cells) {
        vi->pVertexBindingDescriptions = desc->terrain_anim ? &k_terrain_cell_anim_bind
                                                            : &k_terrain_cell_bind;
        vi->vertexAttributeDescriptionCount = desc->terrain_anim ? 16 : 13;
        vi->pVertexAttributeDescriptions = k_terrain_cell_attrs;
    } else if (desc->terrain_anim) {
        vi->vertexBindingDescriptionCount = 2;
        vi->pVertexBindingDescriptions = k_terrain_anim_binds;
        vi->vertexAttributeDescriptionCount = 14;
        vi->pVertexAttributeDescriptions = k_terrain_anim_attrs;
    } else if (desc->terrain_near) {
        vi->vertexBindingDescriptionCount = 2;
        vi->pVertexBindingDescriptions = k_terrain_near_binds;
        vi->vertexAttributeDescriptionCount = 13;
        vi->pVertexAttributeDescriptions = k_terrain_near_attrs;
    }
}

/* The descriptor set layouts of a pipeline described by `desc`, written to
 * out[0..6]; returns how many of them the pipeline layout uses.
 *
 * set 0 (uses_vram) + optional set 1 (uses_vram2, e.g. the diff pass's
 * reference sampler) both reuse the single-binding vram set layout.
 * postfx: sets 0..2 samplers + set 3 the dynamic-UBO pair. terrain: set 0 =
 * VRAM sampler, set 3 = the UBO pair (globals + sector params); sets 1/2 are
 * declared for layout continuity but never bound/used, and terrain keeps its
 * own two blocks at 5/6 — set 3 binding 1 is the mod's, and a set views one
 * buffer per binding. set1_vram_depth replaces set 1 with the two-sampler
 * layout.
 *
 * material: the mod layout PLUS the engine's occlusion inputs, which a
 * material's fragment stage runs before the mod's colour maths. They sit
 * ABOVE the UBO pair (sets 4..6) rather than replacing sets 1/2 so a
 * material's own uniform block stays at set 3 binding 1 — the one part of
 * the layout mod source actually writes. */
static uint32_t pipeline_set_layouts(const rhi_device *dev, const rhi_pipeline_desc *desc,
                                     VkDescriptorSetLayout out[7])
{
    const bool ubo_pair = desc->postfx || desc->mod_pipeline ||
                          desc->material_pipeline || desc->terrain_pipeline;
    const VkDescriptorSetLayout high = desc->terrain_pipeline ? dev->ubo1_set_layout
                                                              : dev->vram_set_layout;
    for (int i = 0; i < 7; i++) out[i] = dev->vram_set_layout;
    out[3] = ubo_pair ? dev->ubo_set_layout : dev->vram_set_layout;
    out[5] = high;
    out[6] = high;
    if (desc->set1_vram_depth) out[1] = dev->pair_set_layout;
    uint32_t n = desc->material_pipeline ? 7u  /* + sets 4..6: og keys, rank */
               : desc->terrain_pipeline ? 7u   /* + 4: HD atlas, 5/6: blocks */
               : (desc->postfx || desc->mod_pipeline) ? 4u
               : desc->uses_vram5 ? 5u
               : desc->uses_vram4 ? 4u
               : desc->uses_vram3 ? 3u
               : desc->uses_vram ? (desc->uses_vram2 ? 2u : 1u) : 0u;
    return (desc->set1_vram_depth && n < 2u) ? 2u : n;
}

/* Compile a pipeline's Vulkan objects from its stored sources (desc + shader
 * names/words) without touching the rhi_pipeline's live handles: the create
 * path assigns the result to a fresh pipeline, the rebuild path swaps it into
 * a live one only on success. Returns 0 on success. */
static int pipeline_compile(rhi_device *dev, rhi_pipeline *p,
                            VkPipeline *out_pipe, VkPipelineLayout *out_layout)
{
    const rhi_pipeline_desc *desc = &p->desc;
    VkShaderModule vs = p->vert_words
        ? create_shader_module(dev, p->vert_words, p->vert_n * 4, "(mod vert)")
        : load_shader_module(dev, desc->vert_spv);
    VkShaderModule fs = p->frag_words
        ? create_shader_module(dev, p->frag_words, p->frag_n * 4, "(mod frag)")
        : load_shader_module(dev, desc->frag_spv);
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        if (vs) vkDestroyShaderModule(dev->device, vs, NULL);
        if (fs) vkDestroyShaderModule(dev->device, fs, NULL);
        return -1;
    }
    VkPipelineLayout layout = VK_NULL_HANDLE;

    VkSpecializationMapEntry vsSpecMap[6] = {
        { .constantID = 0, .offset = 0,                    .size = sizeof(VkBool32) },
        { .constantID = 1, .offset = sizeof(VkBool32),     .size = sizeof(VkBool32) },
        { .constantID = 2, .offset = 2 * sizeof(VkBool32), .size = sizeof(VkBool32) },
        { .constantID = 3, .offset = 3 * sizeof(VkBool32), .size = sizeof(VkBool32) },
        { .constantID = 4, .offset = 4 * sizeof(VkBool32), .size = sizeof(VkBool32) },
        { .constantID = 5, .offset = 5 * sizeof(VkBool32), .size = sizeof(VkBool32) },
    };
    /* Constant 5 (`STP_SELECT`) is per pipeline, from its desc: the texel STP
     * selection of psx_scene.frag (rhi_pipeline_desc.texel_stp_select). */
    VkBool32 vsSpecData[6] = { dev->depth_rev[desc->depth_class], s_vs_affine_uv,
                               pipeline_notex(desc),
                               s_fs_moby_truescope, s_fs_slot_contest,
                               (VkBool32)desc->texel_stp_select };
    VkSpecializationInfo vsSpec = {
        .mapEntryCount = 6, .pMapEntries = vsSpecMap,
        .dataSize = sizeof(vsSpecData), .pData = vsSpecData,
    };
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,   .module = vs, .pName = "main",
          .pSpecializationInfo = &vsSpec },
        /* The fragment stage sees the SAME spec constants (constant_id 0 =
         * DEPTH_REVERSED): the terrain key-resolve fragments invert
         * gl_FragCoord.z back to view-Z, and the inversion formula follows
         * the encoding the vertex stage selected. Frags that declare no
         * spec constants ignore the info. */
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main",
          .pSpecializationInfo = &vsSpec },
    };
    VkVertexInputBindingDescription bind = {
        .binding = 0, .stride = sizeof(rhi_vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    /* The vertex STRUCT keeps all fields (stable buffer layout), but a pipeline
     * only needs to declare the attributes its shaders consume — an attribute
     * declared here yet not read by a shader merely warns "not consumed", so one
     * superset attribute list serves every pipeline. Location 3 (the moby paint
     * rank/ordinal pair) is read by the scene vertex stages' same-moby painter
     * select; every other shader ignores it. Location 4 is the perspective W (true
     * view-Z), read by the scene vertex stages. Location 5 is the terrain
     * og-key slot offset (rhi_vertex.koff), read by the scene vertex stages
     * and the terrain key pass. */
    VkVertexInputAttributeDescription attrs[6] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(rhi_vertex, x) },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,    .offset = offsetof(rhi_vertex, u) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UNORM,   .offset = offsetof(rhi_vertex, rgba) },
        { .location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,    .offset = offsetof(rhi_vertex, sx) },
        { .location = 4, .binding = 0, .format = VK_FORMAT_R32_SFLOAT,       .offset = offsetof(rhi_vertex, w) },
        { .location = 5, .binding = 0, .format = VK_FORMAT_R32_SFLOAT,       .offset = offsetof(rhi_vertex, koff) },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bind,
        .vertexAttributeDescriptionCount = 6, .pVertexAttributeDescriptions = attrs,
    };
    if (desc->terrain_pipeline) terrain_vertex_input(desc, &vi);
    if (desc->postfx) {   /* fullscreen triangle: no vertex buffer at all */
        vi.vertexBindingDescriptionCount = 0;
        vi.pVertexBindingDescriptions = NULL;
        vi.vertexAttributeDescriptionCount = 0;
        vi.pVertexAttributeDescriptions = NULL;
    }
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
        .polygonMode = pipeline_polygon_mode(dev, desc),
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        /* Decal pipelines (desc->depth_bias) enable the hardware polygon
         * offset with DYNAMIC factors (VK_DYNAMIC_STATE_DEPTH_BIAS below);
         * both pass begins set the factors to 0 so state is always defined. */
        .depthBiasEnable = desc->depth_bias ? VK_TRUE : VK_FALSE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState cba = {
        .blendEnable = (desc->blend != RHI_BLEND_NONE) ? VK_TRUE : VK_FALSE,
        /* color_write_off = an invisible pass (stencil-mask rasterization). */
        .colorWriteMask = desc->color_write_off ? 0u :
                          (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT),
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
    } else if (desc->blend == RHI_BLEND_ADD_QUARTER) {
        /* PSX ABR mode 3: dst = B + F/4. The quarter rides the pipeline's
         * static blend constant, so no shader or vertex scaling is involved
         * and the source keeps its own clamp. */
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    } else {
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    }
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba,
        .blendConstants = { 0.25f, 0.25f, 0.25f, 0.25f },
    };
    /* Depth: the offscreen target always carries a depth attachment (3a.3);
     * a pipeline opts into testing/writing it via desc->depth_test. The 2D
     * panels (depth_test=false) ignore it and never write it. The compare is
     * the pipeline's depth class's (depth_convention_init), an OR_EQUAL op so
     * equal-depth later draws (e.g. coincident prims) still paint.
     * depth_overwrite swaps the compare for ALWAYS (unconditional replace);
     * the test stays ENABLED because Vulkan forces depth writes off whenever
     * depthTestEnable is VK_FALSE. */
    const VkCompareOp cmp = desc->depth_overwrite ? VK_COMPARE_OP_ALWAYS
                                                  : dev->depth_cmp[desc->depth_class];
    dev->pipes_by_cmp[cmp] += (unsigned)desc->depth_test;
    VkPipelineDepthStencilStateCreateInfo dss = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable  = desc->depth_test ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = (desc->depth_test && !desc->depth_write_off) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = cmp,
    };
    /* Stencil roles (W2.6): MARK writes 1 where the fragment passes the depth
     * test (depth-fail keeps 0, so nearer world geometry punches the mask
     * out); TEST draws only where stencil==1 and never writes it. Reference is
     * a fixed 1 — per-object isolation is rhi_clear_stencil between groups. */
    if (desc->stencil == RHI_STENCIL_MARK) {
        VkStencilOpState sop = {
            .failOp = VK_STENCIL_OP_KEEP, .passOp = VK_STENCIL_OP_REPLACE,
            .depthFailOp = VK_STENCIL_OP_KEEP, .compareOp = VK_COMPARE_OP_ALWAYS,
            .compareMask = 0xFFu, .writeMask = 0xFFu, .reference = 1u,
        };
        dss.stencilTestEnable = VK_TRUE;
        dss.front = sop; dss.back = sop;
    } else if (desc->stencil == RHI_STENCIL_TEST) {
        VkStencilOpState sop = {
            .failOp = VK_STENCIL_OP_KEEP, .passOp = VK_STENCIL_OP_KEEP,
            .depthFailOp = VK_STENCIL_OP_KEEP, .compareOp = VK_COMPARE_OP_EQUAL,
            .compareMask = 0xFFu, .writeMask = 0u, .reference = 1u,
        };
        dss.stencilTestEnable = VK_TRUE;
        dss.front = sop; dss.back = sop;
    } else if (desc->stencil == RHI_STENCIL_TEST_ZERO) {
        /* Pass where stencil != 1, i.e. ==0 (only 0/1 are ever written): the
         * hole-fill role — paint only pixels the MARK pass left uncovered. */
        VkStencilOpState sop = {
            .failOp = VK_STENCIL_OP_KEEP, .passOp = VK_STENCIL_OP_KEEP,
            .depthFailOp = VK_STENCIL_OP_KEEP, .compareOp = VK_COMPARE_OP_NOT_EQUAL,
            .compareMask = 0xFFu, .writeMask = 0u, .reference = 1u,
        };
        dss.stencilTestEnable = VK_TRUE;
        dss.front = sop; dss.back = sop;
    }
    VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                              VK_DYNAMIC_STATE_DEPTH_BIAS };
    VkPipelineDynamicStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = desc->depth_bias ? 3u : 2u, .pDynamicStates = dyn,
    };
    /* Optional VRAM sampler set (0) + a fragment-stage push-constant block
     * (the per-draw PSX material: tpage/clut/bpp). The 1b.1 quad pipeline
     * uses neither and keeps an empty layout. */
    VkPushConstantRange pcr = {
        /* Terrain pipelines read the per-draw block (clip affine + class
         * select) in the VERTEX stage too; everything else is fragment-only
         * (the per-draw PSX material). */
        .stageFlags = desc->terrain_pipeline
                          ? (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                          : VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0, .size = desc->push_const_size,
    };
    VkDescriptorSetLayout set_layouts[7];
    uint32_t set_count = pipeline_set_layouts(dev, desc, set_layouts);
    if (set_count > dev->max_desc_sets) {
        /* Degrade, never crash: the caller treats a NULL pipeline as "draw
         * this run stock", so a device too small for the material layout
         * loses reshading rather than the frame. */
        RHI_LOG("pipeline needs %u descriptor sets, device allows %u\n",
                set_count, dev->max_desc_sets);
        vkDestroyShaderModule(dev->device, vs, NULL);
        vkDestroyShaderModule(dev->device, fs, NULL);
        return -1;
    }
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = set_count,
        .pSetLayouts = set_count ? set_layouts : NULL,
        .pushConstantRangeCount = desc->push_const_size ? 1u : 0u,
        .pPushConstantRanges = desc->push_const_size ? &pcr : NULL,
    };
    if (vkCreatePipelineLayout(dev->device, &plci, NULL, &layout) != VK_SUCCESS) {
        RHI_LOG("vkCreatePipelineLayout failed\n");
        vkDestroyShaderModule(dev->device, vs, NULL);
        vkDestroyShaderModule(dev->device, fs, NULL);
        return -1;
    }
    p->pc_stages = desc->push_const_size ? pcr.stageFlags : 0;
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
        .layout = layout,
        .renderPass = dev->off_rp,
        .subpass = 0,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(dev->device, VK_NULL_HANDLE, 1, &gp, NULL, &pipeline);
    vkDestroyShaderModule(dev->device, vs, NULL);
    vkDestroyShaderModule(dev->device, fs, NULL);
    if (r != VK_SUCCESS) {
        RHI_LOG("vkCreateGraphicsPipelines failed: %d\n", (int)r);
        vkDestroyPipelineLayout(dev->device, layout, NULL);
        return -1;
    }
    *out_pipe = pipeline;
    *out_layout = layout;
    return 0;
}

/* Common constructor: store owned copies of the shader sources on the
 * pipeline (rebuild inputs), compile, and link into the device's rebuild
 * registry. Word buffers (mod SPIR-V) take precedence over filenames. */
static rhi_pipeline *create_pipeline_common(rhi_device *dev,
                                            const rhi_pipeline_desc *desc,
                                            const uint32_t *vert_words, size_t vert_n,
                                            const uint32_t *frag_words, size_t frag_n)
{
    rhi_pipeline *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->desc = *desc;
    if (vert_words && vert_n) {
        p->vert_words = malloc(vert_n * 4);
        if (!p->vert_words) goto fail;
        memcpy(p->vert_words, vert_words, vert_n * 4);
        p->vert_n = vert_n;
        p->desc.vert_spv = NULL;
    } else if (desc->vert_spv) {
        p->vert_name = strdup(desc->vert_spv);
        if (!p->vert_name) goto fail;
        p->desc.vert_spv = p->vert_name;
    }
    if (frag_words && frag_n) {
        p->frag_words = malloc(frag_n * 4);
        if (!p->frag_words) goto fail;
        memcpy(p->frag_words, frag_words, frag_n * 4);
        p->frag_n = frag_n;
        p->desc.frag_spv = NULL;
    } else if (desc->frag_spv) {
        p->frag_name = strdup(desc->frag_spv);
        if (!p->frag_name) goto fail;
        p->desc.frag_spv = p->frag_name;
    }
    if (pipeline_compile(dev, p, &p->pipeline, &p->layout) != 0) goto fail;
    p->next = dev->pipes_head;
    if (p->next) p->next->prev = p;
    dev->pipes_head = p;
    return p;
fail:
    free(p->vert_name); free(p->frag_name);
    free(p->vert_words); free(p->frag_words);
    free(p);
    return NULL;
}

rhi_pipeline *rhi_create_pipeline(rhi_device *dev, const rhi_pipeline_desc *desc)
{
    return create_pipeline_common(dev, desc, NULL, 0, NULL, 0);
}

rhi_pipeline *rhi_create_pipeline_spv(rhi_device *dev,
                                      const rhi_pipeline_desc *desc,
                                      const uint32_t *vert_words, size_t vert_n,
                                      const uint32_t *frag_words, size_t frag_n)
{
    return create_pipeline_common(dev, desc, vert_words, vert_n,
                                  frag_words, frag_n);
}

int rhi_rebuild_pipelines(rhi_device *dev)
{
    if (!dev) return 0;
    vkDeviceWaitIdle(dev->device);   /* no pipeline may be in flight during the swap */
    int fails = 0, rebuilt = 0;
    for (rhi_pipeline *p = dev->pipes_head; p; p = p->next) {
        VkPipeline np = VK_NULL_HANDLE;
        VkPipelineLayout nl = VK_NULL_HANDLE;
        if (pipeline_compile(dev, p, &np, &nl) == 0) {
            if (p->pipeline) vkDestroyPipeline(dev->device, p->pipeline, NULL);
            if (p->layout)   vkDestroyPipelineLayout(dev->device, p->layout, NULL);
            p->pipeline = np;
            p->layout = nl;
            rebuilt++;
        } else {
            fails++;   /* keep the old objects: stale spec constants beat a hole */
        }
    }
    RHI_LOG("rebuilt %d pipelines (%d failed)\n", rebuilt, fails);
    return fails;
}

/* ---- rhi_ubo: fixed-size uniform blocks × frame ring (S2) ---------- */

/* Generations per frame slot for a uniform block: a write to a block that a
 * draw in the same frame already bound moves to the next generation
 * (physical block = (frame * RHI_UBO_GENS + gen) * blocks + block), so the
 * recorded draw keeps the data it was bound to. A block written more than
 * RHI_UBO_GENS times after binds in one frame overwrites the last generation
 * in place (counted in rhi_ring_gen_overflows). */
#define RHI_UBO_GENS 4u

struct rhi_ubo {
    rhi_buffer     *buf;         /* HOST_VISIBLE|COHERENT, UNIFORM usage */
    uint32_t        block_size;  /* aligned to minUniformBufferOffsetAlignment */
    uint32_t        blocks;      /* per frame slot */
    uint8_t        *gen;         /* [blocks] current generation this frame */
    uint8_t        *bound;       /* [blocks] generation referenced by a draw */
    uint64_t        gen_serial;  /* frame serial gen[]/bound[] belong to */
    VkDescriptorSet set;         /* 2 dynamic-UBO bindings into buf */
    VkDescriptorSet set1;        /* 1 dynamic-UBO binding (rhi_ubo_bind_at) */
    rhi_device     *dev;
};

rhi_ubo *rhi_create_ubo(rhi_device *dev, uint32_t block_size, uint32_t blocks)
{
    if (!dev || block_size == 0 || blocks == 0) return NULL;
    rhi_ubo *u = calloc(1, sizeof(*u));
    if (!u) return NULL;
    u->dev = dev;
    u->blocks = blocks;
    u->block_size = (uint32_t)((block_size + dev->ubo_align - 1) &
                               ~(dev->ubo_align - 1));
    size_t total = (size_t)u->block_size * blocks * RHI_FRAMES_IN_FLIGHT * RHI_UBO_GENS;
    u->buf = create_host_buffer(dev, total, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                /*prefer_cached=*/false);
    u->gen = calloc(blocks, 1);
    u->bound = calloc(blocks, 1);
    if (!u->buf || !u->gen || !u->bound) {
        if (u->buf) rhi_destroy_buffer(dev, u->buf);
        free(u->gen); free(u->bound); free(u); return NULL;
    }
    memset(u->buf->mapped, 0, total);   /* unwritten blocks read as zeros */

    VkDescriptorSetAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dev->desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &dev->ubo_set_layout,
    };
    if (vkAllocateDescriptorSets(dev->device, &ai, &u->set) != VK_SUCCESS) {
        RHI_LOG("rhi_create_ubo: descriptor alloc failed\n");
        rhi_destroy_buffer(dev, u->buf);
        free(u); return NULL;
    }
    /* Both bindings view the same buffer; the dynamic offsets at bind time
     * select (frame slot, block) per binding. Range = one block. */
    VkDescriptorBufferInfo bi = {
        .buffer = u->buf->buffer, .offset = 0, .range = u->block_size,
    };
    VkWriteDescriptorSet wr[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = u->set,
          .dstBinding = 0, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
          .pBufferInfo = &bi },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = u->set,
          .dstBinding = 1, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
          .pBufferInfo = &bi },
    };
    vkUpdateDescriptorSets(dev->device, 2, wr, 0, NULL);

    /* Second view of the same buffer through the single-binding layout, so a
     * block can also be bound at a set index outside the contract set. Its
     * absence is not fatal — rhi_ubo_bind_at simply becomes a no-op. */
    ai.pSetLayouts = &dev->ubo1_set_layout;
    if (vkAllocateDescriptorSets(dev->device, &ai, &u->set1) == VK_SUCCESS) {
        VkWriteDescriptorSet w1 = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = u->set1,
            .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            .pBufferInfo = &bi,
        };
        vkUpdateDescriptorSets(dev->device, 1, &w1, 0, NULL);
    } else {
        RHI_LOG("rhi_create_ubo: single-binding descriptor alloc failed\n");
        u->set1 = VK_NULL_HANDLE;
    }
    return u;
}

/* Reset the per-block generation state on the first touch of a new frame. */
static void ubo_frame_sync(rhi_ubo *ubo)
{
    if (ubo->gen_serial == s_frame_serial) return;
    ubo->gen_serial = s_frame_serial;
    memset(ubo->gen, 0, ubo->blocks);
    memset(ubo->bound, 0, ubo->blocks);
}
static size_t ubo_phys(const rhi_ubo *ubo, uint32_t block)
{
    return ((size_t)ubo->dev->frame * RHI_UBO_GENS + ubo->gen[block]) * ubo->blocks + block;
}

void rhi_ubo_write(rhi_ubo *ubo, uint32_t block, const void *data, uint32_t len)
{
    if (!ubo || !data || block >= ubo->blocks) return;
    if (len > ubo->block_size) len = ubo->block_size;
    if (ubo->dev->frame_active) {
        ubo_frame_sync(ubo);
        if (ubo->bound[block]) {
            if (ubo->gen[block] + 1u < RHI_UBO_GENS) { ubo->gen[block]++; s_ring_gen_bumps++; }
            else {
                s_ring_gen_overflows++;
                static bool once; if (!once) { once = true;
                    RHI_LOG("ubo block %u: %u generations exhausted in frame %llu; overwriting bound data\n",
                            block, RHI_UBO_GENS, (unsigned long long)s_frame_serial); }
            }
            ubo->bound[block] = 0;
        }
        memcpy((uint8_t *)ubo->buf->mapped + ubo_phys(ubo, block) * ubo->block_size,
               data, len);
    } else {
        /* Outside a frame: fill every parity and generation (init-time writes). */
        for (uint32_t i = 0; i < RHI_FRAMES_IN_FLIGHT * RHI_UBO_GENS; i++)
            memcpy((uint8_t *)ubo->buf->mapped +
                       ((size_t)i * ubo->blocks + block) * ubo->block_size,
                   data, len);
    }
}

void rhi_ubo_bind(rhi_device *dev, rhi_ubo *ubo, uint32_t block_a,
                  uint32_t block_b)
{
    if (!dev || !ubo || !dev->frame_active || !dev->cur_layout) return;
    if (block_a >= ubo->blocks || block_b >= ubo->blocks) return;
    ubo_frame_sync(ubo);
    ubo->bound[block_a] = ubo->bound[block_b] = 1;
    uint32_t offs[2] = {
        (uint32_t)ubo_phys(ubo, block_a) * ubo->block_size,
        (uint32_t)ubo_phys(ubo, block_b) * ubo->block_size,
    };
    vkCmdBindDescriptorSets(dev->cmd[dev->frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                            dev->cur_layout, 3, 1, &ubo->set, 2, offs);
}

void rhi_ubo_bind_at(rhi_device *dev, rhi_ubo *ubo, uint32_t set, uint32_t block)
{
    if (!dev || !ubo || !ubo->set1 || !dev->frame_active || !dev->cur_layout) return;
    if (block >= ubo->blocks) return;
    ubo_frame_sync(ubo);
    ubo->bound[block] = 1;
    uint32_t off = (uint32_t)ubo_phys(ubo, block) * ubo->block_size;
    vkCmdBindDescriptorSets(dev->cmd[dev->frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                            dev->cur_layout, set, 1, &ubo->set1, 1, &off);
}

void rhi_destroy_ubo(rhi_device *dev, rhi_ubo *ubo)
{
    if (!ubo) return;
    if (ubo->set1) vkFreeDescriptorSets(dev->device, dev->desc_pool, 1, &ubo->set1);
    if (ubo->set) vkFreeDescriptorSets(dev->device, dev->desc_pool, 1, &ubo->set);
    rhi_destroy_buffer(dev, ubo->buf);
    free(ubo->gen); free(ubo->bound);
    free(ubo);
}

/* ---- offscreen-as-input: the COMPOSITE postfx point (S2) ----------- */

void rhi_offscreen_to_sampled(rhi_device *dev)
{
    if (!dev || !dev->frame_active) return;
    /* off_rp left the image TRANSFER_SRC (its finalLayout); its subpass->
     * external dependency covers TRANSFER reads only, so raise our own
     * color-write -> fragment-read barrier with the layout flip. */
    image_barrier(dev->cmd[dev->frame], dev->off_image,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

void rhi_bind_offscreen_tex(rhi_device *dev, uint32_t set)
{
    if (!dev || !dev->frame_active || !dev->cur_layout) return;
    uint32_t slot = dev->frame;
    if (dev->off_sampled_set[slot] == VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = dev->desc_pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &dev->vram_set_layout,
        };
        if (vkAllocateDescriptorSets(dev->device, &ai,
                                     &dev->off_sampled_set[slot]) != VK_SUCCESS) {
            RHI_LOG("off_sampled_set alloc failed\n");
            return;
        }
        dev->off_sampled_gen[slot] = 0;   /* force the write below */
    }
    if (dev->off_sampled_gen[slot] != dev->off_gen) {
        /* Safe to rewrite THIS slot's set: begin_frame fenced this slot, so
         * no in-flight use remains (the other slot keeps its own set). */
        VkDescriptorImageInfo ii = {
            .sampler = dev->linear_sampler,
            .imageView = dev->off_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet wr = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = dev->off_sampled_set[slot],
            .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &ii,
        };
        vkUpdateDescriptorSets(dev->device, 1, &wr, 0, NULL);
        dev->off_sampled_gen[slot] = dev->off_gen;
    }
    vkCmdBindDescriptorSets(dev->cmd[dev->frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                            dev->cur_layout, set, 1,
                            &dev->off_sampled_set[slot], 0, NULL);
}

/* Undo rhi_offscreen_to_sampled WITHOUT re-rendering (a chain that
 * built no runnable pass): flip off_image straight back to TRANSFER_SRC
 * so the present blit can read it. */
void rhi_offscreen_resume_blit_src(rhi_device *dev)
{
    if (!dev || !dev->frame_active) return;
    image_barrier(dev->cmd[dev->frame], dev->off_image,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
}

void rhi_offscreen_resume_attachment(rhi_device *dev)
{
    if (!dev || !dev->frame_active) return;
    /* Fragment reads of off_image must finish before the re-begun offscreen
     * pass writes it (off_rp initialLayout is UNDEFINED + loadOp CLEAR, so
     * the layout value here only names the source scope). */
    image_barrier(dev->cmd[dev->frame], dev->off_image,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
}

void rhi_bind_texture_set(rhi_device *dev, rhi_texture *tex, uint32_t set)
{
    if (!dev || !tex || !dev->frame_active || !dev->cur_layout) return;
    vkCmdBindDescriptorSets(dev->cmd[dev->frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                            dev->cur_layout, set, 1, &tex->set, 0, NULL);
}

/* Postfx chain write-back (S3): 1:1 blit of the chain's final ping RT
 * back into off_image, leaving off_image TRANSFER_SRC for the present
 * blit and the source RT sampleable again. Expects off_image in
 * SHADER_READ (rhi_offscreen_to_sampled ran) and src post-rt-pass
 * (SHADER_READ). */
void rhi_blit_rt_to_offscreen(rhi_device *dev, rhi_texture *src)
{
    if (!dev || !src || !src->is_rt || !dev->frame_active) return;
    VkCommandBuffer cb = dev->cmd[dev->frame];
    image_barrier(cb, src->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    image_barrier(cb, dev->off_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit region = {
        .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .srcOffsets = { {0, 0, 0}, {(int32_t)src->width, (int32_t)src->height, 1} },
        .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .dstOffsets = { {0, 0, 0}, {(int32_t)dev->sc_extent.width,
                                    (int32_t)dev->sc_extent.height, 1} },
    };
    vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dev->off_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region, VK_FILTER_NEAREST);
    image_barrier(cb, dev->off_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    image_barrier(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    src->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

/* Same write-back for a render-target compose path (the screenshot-all
 * native_rtt dump): dst ends SHADER_READ (readable by
 * rhi_read_render_target), src restored to SHADER_READ. */
void rhi_blit_rt_to_rt(rhi_device *dev, rhi_texture *dst, rhi_texture *src)
{
    if (!dev || !dst || !src || !dst->is_rt || !src->is_rt || !dev->frame_active)
        return;
    VkCommandBuffer cb = dev->cmd[dev->frame];
    image_barrier(cb, src->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    image_barrier(cb, dst->image, dst->layout,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit region = {
        .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .srcOffsets = { {0, 0, 0}, {(int32_t)src->width, (int32_t)src->height, 1} },
        .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .dstOffsets = { {0, 0, 0}, {(int32_t)dst->width, (int32_t)dst->height, 1} },
    };
    vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region, VK_FILTER_NEAREST);
    image_barrier(cb, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    image_barrier(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    dst->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    src->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void rhi_destroy_pipeline(rhi_device *dev, rhi_pipeline *pipe)
{
    if (!pipe) return;
    /* Unlink from the rebuild registry (a pipeline with no prev is the head). */
    if (pipe->prev) pipe->prev->next = pipe->next;
    else            dev->pipes_head = pipe->next;
    if (pipe->next) pipe->next->prev = pipe->prev;
    if (pipe->pipeline) vkDestroyPipeline(dev->device, pipe->pipeline, NULL);
    if (pipe->layout)   vkDestroyPipelineLayout(dev->device, pipe->layout, NULL);
    free(pipe->vert_name); free(pipe->frag_name);
    free(pipe->vert_words); free(pipe->frag_words);
    free(pipe);
}

void rhi_bind_pipeline(rhi_device *dev, rhi_pipeline *pipe)
{
    if (!dev->frame_active) return;
    dev->cur_layout = pipe->layout; /* for bind_vram / push_constants */
    dev->cur_pc_stages = pipe->pc_stages;
    vkCmdBindPipeline(dev->cmd[dev->frame], VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->pipeline);
}

void rhi_bind_vertex_buffer(rhi_device *dev, rhi_buffer *buf)
{
    if (!dev->frame_active) return;
    /* Dynamic ring buffers bind the current frame's slot and generation
     * (see rhi_buffer); a static buffer has ring_stride 0. */
    VkDeviceSize off = buf->ring_stride ? buf->ring_stride * ring_bind_slot(buf) : 0;
    vkCmdBindVertexBuffers(dev->cmd[dev->frame], 0, 1, &buf->buffer, &off);
}

void rhi_bind_vertex_buffer2(rhi_device *dev, rhi_buffer *buf, rhi_buffer *prev)
{
    if (!dev->frame_active) return;
    VkBuffer bufs[2] = { buf->buffer, prev->buffer };
    VkDeviceSize offs[2] = { buf->ring_stride ? buf->ring_stride * ring_bind_slot(buf) : 0,
                             prev->ring_stride ? prev->ring_stride * ring_bind_slot(prev) : 0 };
    vkCmdBindVertexBuffers(dev->cmd[dev->frame], 0, 2, bufs, offs);
}

void rhi_texture_set_filter_linear(rhi_device *dev, rhi_texture *tex)
{
    /* Rewrite the texture's (view, sampler) descriptor with the LINEAR
     * sampler. Only safe while the set is not in flight — call at load time,
     * before the texture's first frame. */
    VkDescriptorImageInfo ii = {
        .sampler = dev->linear_sampler,
        .imageView = tex->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = tex->set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &ii,
    };
    vkUpdateDescriptorSets(dev->device, 1, &w, 0, NULL);
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

void rhi_bind_vram_depth_pair(rhi_device *dev, rhi_texture *tex, rhi_texture *depth_rt,
                              uint32_t set)
{
    if (!dev->frame_active || !tex || !depth_rt || !depth_rt->rt_depth_sample_view) return;
    uint32_t f = dev->frame;
    if (dev->pair_used[f] == dev->pair_alloc[f]) {
        if (dev->pair_alloc[f] == RHI_PAIR_SETS_MAX) {
            static bool s_warned;
            if (!s_warned) {
                RHI_LOG("pair sets: %d binds in one frame exceed the slot list; "
                        "further binds this frame are dropped\n", RHI_PAIR_SETS_MAX);
                s_warned = true;
            }
            return;
        }
        VkDescriptorSetAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = dev->desc_pool, .descriptorSetCount = 1,
            .pSetLayouts = &dev->pair_set_layout,
        };
        if (vkAllocateDescriptorSets(dev->device, &ai,
                                     &dev->pair_sets[f][dev->pair_alloc[f]]) != VK_SUCCESS) {
            RHI_LOG("pair set vkAllocateDescriptorSets failed\n");
            return;
        }
        dev->pair_alloc[f]++;
    }
    VkDescriptorSet ds = dev->pair_sets[f][dev->pair_used[f]++];
    VkDescriptorImageInfo ii[2] = {
        { .sampler = dev->vram_sampler, .imageView = tex->view,
          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { .sampler = dev->vram_sampler, .imageView = depth_rt->rt_depth_sample_view,
          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
    };
    VkWriteDescriptorSet w[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &ii[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &ii[1] },
    };
    vkUpdateDescriptorSets(dev->device, 2, w, 0, NULL);
    vkCmdBindDescriptorSets(dev->cmd[f], VK_PIPELINE_BIND_POINT_GRAPHICS,
                            dev->cur_layout, set, 1, &ds, 0, NULL);
}

void rhi_push_constants(rhi_device *dev, const void *data, uint32_t size)
{
    if (!dev->frame_active) return;
    vkCmdPushConstants(dev->cmd[dev->frame], dev->cur_layout,
                       dev->cur_pc_stages ? dev->cur_pc_stages
                                          : VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, size, data);
}

void rhi_draw(rhi_device *dev, uint32_t vertex_count, uint32_t first_vertex)
{
    if (!dev->frame_active) return;
    dev->draw_calls++;
    vkCmdDraw(dev->cmd[dev->frame], vertex_count, 1, first_vertex, 0);
}

void rhi_draw_instanced(rhi_device *dev, uint32_t vertex_count, uint32_t instance_count,
                        uint32_t first_vertex, uint32_t first_instance)
{
    if (!dev->frame_active) return;
    dev->draw_calls++;
    vkCmdDraw(dev->cmd[dev->frame], vertex_count, instance_count, first_vertex,
              first_instance);
}

uint32_t rhi_draw_calls(rhi_device *dev)
{
    return dev->draw_calls;
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

void rhi_set_depth_bias(rhi_device *dev, float constant, float clamp, float slope)
{
    if (!dev->frame_active) return;
    /* Only consumed by pipelines created with desc.depth_bias (the dynamic
     * state is per-pipeline); setting it with another pipeline bound is legal
     * and inert. Parameter order matches vkCmdSetDepthBias. Without the
     * depthBiasClamp feature the clamp param MUST be 0.0 (spec 00790) — the
     * pull is then bounded only by the slope term; acceptable degradation. */
    if (!dev->has_depth_bias_clamp) clamp = 0.0f;
    /* The caller's sign convention is forward: negative pulls toward the
     * camera. Under the reversed scene convention nearer is larger, so all
     * three factors flip. The slope and clamp keep their view-Z meaning
     * exactly (|dz/dvz| = 1/vz^2 in both encodings); the constant is in
     * units of the stored value's resolvable step r, 2^-24 near z = 1
     * forward and about z * 2^-23.5 reversed. */
    const float sgn = dev->depth_rev[RHI_DEPTH_SCENE] ? -1.0f : 1.0f;
    vkCmdSetDepthBias(dev->cmd[dev->frame], sgn * constant, sgn * clamp, sgn * slope);
}
