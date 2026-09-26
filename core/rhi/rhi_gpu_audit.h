/* Guest-execution GPU audit.
 *
 * The sub-tick present grid loses one slot per tick: the present that would
 * land inside the tick's protective margin is refused, because presenting and
 * executing the guest share one thread. Moving presentation to its own thread
 * only works if the guest's own execution never touches the Vulkan device --
 * on the windowed pure-vk play path PsyCross runs library-only (no GL context,
 * no rasterization), so in principle nothing in a tick reaches the RHI. That
 * "in principle" is load-bearing enough to measure rather than assume: the
 * terrain sector cache, for one, is filled from hooks that run inside the
 * guest's render walk.
 *
 * Every Vulkan entry point that mutates device, queue, command-buffer or
 * memory state is redirected through op_gpu_audit_hit() while the guest fiber
 * owns the thread. Query-only calls and one-shot instance/device creation are
 * left alone. Outside guest execution g_openpete_guest_running is 0, so the
 * render path pays one never-taken branch per call.
 *
 * A macro here expands to a comma expression whose second operand is the same
 * name; C does not rescan a macro's own name inside its expansion, so the real
 * function is what gets called. */
#ifndef OPENPETE_RHI_GPU_AUDIT_H
#define OPENPETE_RHI_GPU_AUDIT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Non-zero while the guest fiber is executing a tick on this thread. */
extern int g_openpete_guest_running;

/* Records one Vulkan call made during guest execution. Cheap and lock-free;
 * keeps a total plus the distinct function names seen. */
void op_gpu_audit_hit(const char *fn);
void op_gpu_audit_bind_thread(void);   /* call once from the thread that runs the guest */

/* Second channel: a host write into persistently-mapped device memory made
 * during guest execution. These issue no Vulkan call, so the channel above is
 * blind to them -- but they are exactly the bytes a concurrent present thread
 * would be reading, so they are the hazard that survives a clean call audit.
 * Counted separately so one number can never be mistaken for the other. */
void op_gpu_audit_map_write(size_t bytes);
unsigned op_gpu_audit_map_writes(void);
unsigned long long op_gpu_audit_map_bytes(void);

/** @brief Return the number of Vulkan calls recorded on the guest thread during guest execution.
 *
 * @return The running total since process start. */
unsigned op_gpu_audit_total(void);
/** @brief Copy the distinct Vulkan function names recorded during guest execution.
 *
 * The set holds at most 32 names; names past that are counted in the total
 * but not recorded.
 * @param out Array receiving the name pointers.
 * @param max Capacity of @p out.
 * @return Number of names written to @p out. */
int      op_gpu_audit_names(const char **out, int max);

#ifdef __cplusplus
}
#endif

#define OP_GPU_AUDIT(f) (g_openpete_guest_running ? op_gpu_audit_hit(#f) : (void)0)

#define vkAcquireNextImageKHR(...) (OP_GPU_AUDIT(vkAcquireNextImageKHR), vkAcquireNextImageKHR(__VA_ARGS__))
#define vkAllocateCommandBuffers(...) (OP_GPU_AUDIT(vkAllocateCommandBuffers), vkAllocateCommandBuffers(__VA_ARGS__))
#define vkAllocateDescriptorSets(...) (OP_GPU_AUDIT(vkAllocateDescriptorSets), vkAllocateDescriptorSets(__VA_ARGS__))
#define vkAllocateMemory(...) (OP_GPU_AUDIT(vkAllocateMemory), vkAllocateMemory(__VA_ARGS__))
#define vkBeginCommandBuffer(...) (OP_GPU_AUDIT(vkBeginCommandBuffer), vkBeginCommandBuffer(__VA_ARGS__))
#define vkBindBufferMemory(...) (OP_GPU_AUDIT(vkBindBufferMemory), vkBindBufferMemory(__VA_ARGS__))
#define vkBindImageMemory(...) (OP_GPU_AUDIT(vkBindImageMemory), vkBindImageMemory(__VA_ARGS__))
#define vkCmdBeginRenderPass(...) (OP_GPU_AUDIT(vkCmdBeginRenderPass), vkCmdBeginRenderPass(__VA_ARGS__))
#define vkCmdBindDescriptorSets(...) (OP_GPU_AUDIT(vkCmdBindDescriptorSets), vkCmdBindDescriptorSets(__VA_ARGS__))
#define vkCmdBindPipeline(...) (OP_GPU_AUDIT(vkCmdBindPipeline), vkCmdBindPipeline(__VA_ARGS__))
#define vkCmdBindVertexBuffers(...) (OP_GPU_AUDIT(vkCmdBindVertexBuffers), vkCmdBindVertexBuffers(__VA_ARGS__))
#define vkCmdBlitImage(...) (OP_GPU_AUDIT(vkCmdBlitImage), vkCmdBlitImage(__VA_ARGS__))
#define vkCmdClearAttachments(...) (OP_GPU_AUDIT(vkCmdClearAttachments), vkCmdClearAttachments(__VA_ARGS__))
#define vkCmdCopyBufferToImage(...) (OP_GPU_AUDIT(vkCmdCopyBufferToImage), vkCmdCopyBufferToImage(__VA_ARGS__))
#define vkCmdCopyImageToBuffer(...) (OP_GPU_AUDIT(vkCmdCopyImageToBuffer), vkCmdCopyImageToBuffer(__VA_ARGS__))
#define vkCmdDraw(...) (OP_GPU_AUDIT(vkCmdDraw), vkCmdDraw(__VA_ARGS__))
#define vkCmdEndRenderPass(...) (OP_GPU_AUDIT(vkCmdEndRenderPass), vkCmdEndRenderPass(__VA_ARGS__))
#define vkCmdPipelineBarrier(...) (OP_GPU_AUDIT(vkCmdPipelineBarrier), vkCmdPipelineBarrier(__VA_ARGS__))
#define vkCmdPushConstants(...) (OP_GPU_AUDIT(vkCmdPushConstants), vkCmdPushConstants(__VA_ARGS__))
#define vkCmdResetQueryPool(...) (OP_GPU_AUDIT(vkCmdResetQueryPool), vkCmdResetQueryPool(__VA_ARGS__))
#define vkCmdSetDepthBias(...) (OP_GPU_AUDIT(vkCmdSetDepthBias), vkCmdSetDepthBias(__VA_ARGS__))
#define vkCmdSetScissor(...) (OP_GPU_AUDIT(vkCmdSetScissor), vkCmdSetScissor(__VA_ARGS__))
#define vkCmdSetViewport(...) (OP_GPU_AUDIT(vkCmdSetViewport), vkCmdSetViewport(__VA_ARGS__))
#define vkCmdWriteTimestamp(...) (OP_GPU_AUDIT(vkCmdWriteTimestamp), vkCmdWriteTimestamp(__VA_ARGS__))
#define vkCreateBuffer(...) (OP_GPU_AUDIT(vkCreateBuffer), vkCreateBuffer(__VA_ARGS__))
#define vkCreateCommandPool(...) (OP_GPU_AUDIT(vkCreateCommandPool), vkCreateCommandPool(__VA_ARGS__))
#define vkCreateDescriptorPool(...) (OP_GPU_AUDIT(vkCreateDescriptorPool), vkCreateDescriptorPool(__VA_ARGS__))
#define vkCreateDescriptorSetLayout(...) (OP_GPU_AUDIT(vkCreateDescriptorSetLayout), vkCreateDescriptorSetLayout(__VA_ARGS__))
#define vkCreateFence(...) (OP_GPU_AUDIT(vkCreateFence), vkCreateFence(__VA_ARGS__))
#define vkCreateFramebuffer(...) (OP_GPU_AUDIT(vkCreateFramebuffer), vkCreateFramebuffer(__VA_ARGS__))
#define vkCreateGraphicsPipelines(...) (OP_GPU_AUDIT(vkCreateGraphicsPipelines), vkCreateGraphicsPipelines(__VA_ARGS__))
#define vkCreateImage(...) (OP_GPU_AUDIT(vkCreateImage), vkCreateImage(__VA_ARGS__))
#define vkCreateImageView(...) (OP_GPU_AUDIT(vkCreateImageView), vkCreateImageView(__VA_ARGS__))
#define vkCreatePipelineLayout(...) (OP_GPU_AUDIT(vkCreatePipelineLayout), vkCreatePipelineLayout(__VA_ARGS__))
#define vkCreateQueryPool(...) (OP_GPU_AUDIT(vkCreateQueryPool), vkCreateQueryPool(__VA_ARGS__))
#define vkCreateRenderPass(...) (OP_GPU_AUDIT(vkCreateRenderPass), vkCreateRenderPass(__VA_ARGS__))
#define vkCreateSampler(...) (OP_GPU_AUDIT(vkCreateSampler), vkCreateSampler(__VA_ARGS__))
#define vkCreateSemaphore(...) (OP_GPU_AUDIT(vkCreateSemaphore), vkCreateSemaphore(__VA_ARGS__))
#define vkCreateShaderModule(...) (OP_GPU_AUDIT(vkCreateShaderModule), vkCreateShaderModule(__VA_ARGS__))
#define vkCreateSwapchainKHR(...) (OP_GPU_AUDIT(vkCreateSwapchainKHR), vkCreateSwapchainKHR(__VA_ARGS__))
#define vkDestroyBuffer(...) (OP_GPU_AUDIT(vkDestroyBuffer), vkDestroyBuffer(__VA_ARGS__))
#define vkDestroyCommandPool(...) (OP_GPU_AUDIT(vkDestroyCommandPool), vkDestroyCommandPool(__VA_ARGS__))
#define vkDestroyDescriptorPool(...) (OP_GPU_AUDIT(vkDestroyDescriptorPool), vkDestroyDescriptorPool(__VA_ARGS__))
#define vkDestroyDescriptorSetLayout(...) (OP_GPU_AUDIT(vkDestroyDescriptorSetLayout), vkDestroyDescriptorSetLayout(__VA_ARGS__))
#define vkDestroyFence(...) (OP_GPU_AUDIT(vkDestroyFence), vkDestroyFence(__VA_ARGS__))
#define vkDestroyFramebuffer(...) (OP_GPU_AUDIT(vkDestroyFramebuffer), vkDestroyFramebuffer(__VA_ARGS__))
#define vkDestroyImage(...) (OP_GPU_AUDIT(vkDestroyImage), vkDestroyImage(__VA_ARGS__))
#define vkDestroyImageView(...) (OP_GPU_AUDIT(vkDestroyImageView), vkDestroyImageView(__VA_ARGS__))
#define vkDestroyPipeline(...) (OP_GPU_AUDIT(vkDestroyPipeline), vkDestroyPipeline(__VA_ARGS__))
#define vkDestroyPipelineLayout(...) (OP_GPU_AUDIT(vkDestroyPipelineLayout), vkDestroyPipelineLayout(__VA_ARGS__))
#define vkDestroyQueryPool(...) (OP_GPU_AUDIT(vkDestroyQueryPool), vkDestroyQueryPool(__VA_ARGS__))
#define vkDestroyRenderPass(...) (OP_GPU_AUDIT(vkDestroyRenderPass), vkDestroyRenderPass(__VA_ARGS__))
#define vkDestroySampler(...) (OP_GPU_AUDIT(vkDestroySampler), vkDestroySampler(__VA_ARGS__))
#define vkDestroySemaphore(...) (OP_GPU_AUDIT(vkDestroySemaphore), vkDestroySemaphore(__VA_ARGS__))
#define vkDestroyShaderModule(...) (OP_GPU_AUDIT(vkDestroyShaderModule), vkDestroyShaderModule(__VA_ARGS__))
#define vkDestroySwapchainKHR(...) (OP_GPU_AUDIT(vkDestroySwapchainKHR), vkDestroySwapchainKHR(__VA_ARGS__))
#define vkDeviceWaitIdle(...) (OP_GPU_AUDIT(vkDeviceWaitIdle), vkDeviceWaitIdle(__VA_ARGS__))
#define vkEndCommandBuffer(...) (OP_GPU_AUDIT(vkEndCommandBuffer), vkEndCommandBuffer(__VA_ARGS__))
#define vkFreeCommandBuffers(...) (OP_GPU_AUDIT(vkFreeCommandBuffers), vkFreeCommandBuffers(__VA_ARGS__))
#define vkFreeDescriptorSets(...) (OP_GPU_AUDIT(vkFreeDescriptorSets), vkFreeDescriptorSets(__VA_ARGS__))
#define vkFreeMemory(...) (OP_GPU_AUDIT(vkFreeMemory), vkFreeMemory(__VA_ARGS__))
#define vkGetFenceStatus(...) (OP_GPU_AUDIT(vkGetFenceStatus), vkGetFenceStatus(__VA_ARGS__))
#define vkGetQueryPoolResults(...) (OP_GPU_AUDIT(vkGetQueryPoolResults), vkGetQueryPoolResults(__VA_ARGS__))
#define vkMapMemory(...) (OP_GPU_AUDIT(vkMapMemory), vkMapMemory(__VA_ARGS__))
#define vkQueuePresentKHR(...) (OP_GPU_AUDIT(vkQueuePresentKHR), vkQueuePresentKHR(__VA_ARGS__))
#define vkQueueSubmit(...) (OP_GPU_AUDIT(vkQueueSubmit), vkQueueSubmit(__VA_ARGS__))
#define vkQueueWaitIdle(...) (OP_GPU_AUDIT(vkQueueWaitIdle), vkQueueWaitIdle(__VA_ARGS__))
#define vkResetCommandBuffer(...) (OP_GPU_AUDIT(vkResetCommandBuffer), vkResetCommandBuffer(__VA_ARGS__))
#define vkResetFences(...) (OP_GPU_AUDIT(vkResetFences), vkResetFences(__VA_ARGS__))
#define vkUnmapMemory(...) (OP_GPU_AUDIT(vkUnmapMemory), vkUnmapMemory(__VA_ARGS__))
#define vkUpdateDescriptorSets(...) (OP_GPU_AUDIT(vkUpdateDescriptorSets), vkUpdateDescriptorSets(__VA_ARGS__))
#define vkWaitForFences(...) (OP_GPU_AUDIT(vkWaitForFences), vkWaitForFences(__VA_ARGS__))
#define vkWaitForPresentKHR(...) (OP_GPU_AUDIT(vkWaitForPresentKHR), vkWaitForPresentKHR(__VA_ARGS__))

#endif /* OPENPETE_RHI_GPU_AUDIT_H */
