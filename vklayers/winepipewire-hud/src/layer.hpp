/*
 * Vulkan layer core for the winepipewire audio diagnostic overlay.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINEPIPEWIRE_HUD_LAYER_HPP
#define WINEPIPEWIRE_HUD_LAYER_HPP

#include <cstdint>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include "log.hpp"
#include "frame.hpp"
#include "render.hpp"
#include "snapshot.hpp"

#define HUD_LAYER_NAME "VK_LAYER_WINEPIPEWIRE_hud"

#define HUD_INSTANCE_FUNCS(F)                   \
    F(GetInstanceProcAddr)                      \
    F(DestroyInstance)                          \
    F(GetPhysicalDeviceProperties)              \
    F(GetPhysicalDeviceMemoryProperties)        \
    F(GetPhysicalDeviceQueueFamilyProperties)

#define HUD_DEVICE_FUNCS(F)                     \
    F(GetDeviceProcAddr)                        \
    F(DestroyDevice)                            \
    F(DeviceWaitIdle)                           \
    F(GetDeviceQueue)                           \
    F(QueueSubmit)                              \
    F(CreateSwapchainKHR)                       \
    F(DestroySwapchainKHR)                      \
    F(GetSwapchainImagesKHR)                    \
    F(QueuePresentKHR)                          \
    F(CreateFence)                              \
    F(DestroyFence)                             \
    F(WaitForFences)                            \
    F(ResetFences)                              \
    F(CreateSemaphore)                          \
    F(DestroySemaphore)                         \
    F(CreateCommandPool)                        \
    F(DestroyCommandPool)                       \
    F(AllocateCommandBuffers)                   \
    F(FreeCommandBuffers)                       \
    F(BeginCommandBuffer)                       \
    F(EndCommandBuffer)                         \
    F(ResetCommandBuffer)                       \
    F(CreateBuffer)                             \
    F(DestroyBuffer)                            \
    F(GetBufferMemoryRequirements)              \
    F(BindBufferMemory)                         \
    F(CreateImage)                              \
    F(DestroyImage)                             \
    F(GetImageMemoryRequirements)               \
    F(BindImageMemory)                          \
    F(CreateImageView)                          \
    F(DestroyImageView)                         \
    F(AllocateMemory)                           \
    F(FreeMemory)                               \
    F(MapMemory)                                \
    F(UnmapMemory)                              \
    F(FlushMappedMemoryRanges)                  \
    F(CreateSampler)                            \
    F(DestroySampler)                           \
    F(CreateDescriptorPool)                     \
    F(DestroyDescriptorPool)                    \
    F(CreateDescriptorSetLayout)                \
    F(DestroyDescriptorSetLayout)               \
    F(AllocateDescriptorSets)                   \
    F(UpdateDescriptorSets)                     \
    F(CreateShaderModule)                       \
    F(DestroyShaderModule)                      \
    F(CreatePipelineLayout)                     \
    F(DestroyPipelineLayout)                    \
    F(CreateGraphicsPipelines)                  \
    F(DestroyPipeline)                          \
    F(CreateRenderPass)                         \
    F(DestroyRenderPass)                        \
    F(CreateFramebuffer)                        \
    F(DestroyFramebuffer)                       \
    F(CmdPipelineBarrier)                       \
    F(CmdCopyBufferToImage)                     \
    F(CmdBeginRenderPass)                       \
    F(CmdEndRenderPass)                         \
    F(CmdBindPipeline)                          \
    F(CmdBindDescriptorSets)                    \
    F(CmdBindVertexBuffers)                     \
    F(CmdBindIndexBuffer)                       \
    F(CmdSetViewport)                           \
    F(CmdSetScissor)                            \
    F(CmdPushConstants)                         \
    F(CmdDrawIndexed)

#define HUD_DISPATCH_FIELD(name) PFN_vk##name name;

struct instance_dispatch
{
    HUD_INSTANCE_FUNCS(HUD_DISPATCH_FIELD)
};

struct device_dispatch
{
    HUD_DEVICE_FUNCS(HUD_DISPATCH_FIELD)
};

#undef HUD_DISPATCH_FIELD

struct instance_data
{
    VkInstance instance;
    struct instance_dispatch vtable;
};

struct device_data
{
    VkDevice device;
    struct instance_data *instance;
    VkPhysicalDevice physical_device;
    /* Held for limits.nonCoherentAtomSize, which the vertex and index buffer
     * sizes must be rounded up to before they are flushed. */
    VkPhysicalDeviceProperties properties;
    struct device_dispatch vtable;
    /* The loader hands this over during vkCreateDevice and nowhere else, so it
     * is captured there or never: layer-allocated command buffers have to be
     * passed through it before they are usable. */
    PFN_vkSetDeviceLoaderData set_device_loader_data;
    struct hud_device_render render;
};

struct swapchain_data
{
    VkSwapchainKHR swapchain;
    struct device_data *device;
    VkExtent2D extent;
    VkFormat format;
    VkColorSpaceKHR color_space;
    uint32_t image_count;
    /* False when the layer had to add VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
     * itself, which is the case that makes the create-info copy necessary. */
    bool app_color_attachment;
    /* vkQueuePresentKHR externally synchronises the swapchain it is given, so
     * these, the snapshot copies and the renderer state are owned by whichever
     * thread presents and need no lock. */
    uint64_t presents;
    uint64_t report_ns;
    uint64_t report_presents;
    struct hud_snapshot_view snapshot;
    struct hud_frame_state frame;
    struct hud_swapchain_render render;
};

struct device_data *device_data_find(void *key);

struct hud_queue *hud_queue_register(struct device_data *device_data, VkQueue queue,
                                     VkQueueFlags flags, uint32_t family_index);
struct hud_queue *hud_queue_find(VkQueue queue);
void hud_queue_unregister(VkQueue queue);

struct swapchain_data *swapchain_data_create(VkSwapchainKHR swapchain, struct device_data *device,
                                            const VkSwapchainCreateInfoKHR *create_info,
                                            bool app_color_attachment);
struct swapchain_data *swapchain_data_find(VkSwapchainKHR swapchain);
void swapchain_data_destroy(VkSwapchainKHR swapchain);

/* The loader writes its dispatch table pointer into the first word of every
 * dispatchable handle, and every object created from a device carries the
 * device's, so a VkQueue resolves to the same device_data as its VkDevice. */
static inline void *dispatch_key(void *object)
{
    return *(void **)object;
}

#define HUD_VK(expr)                                                    \
    do {                                                                \
        VkResult result_ = (expr);                                      \
                                                                        \
        if (result_ != VK_SUCCESS)                                      \
            HUD_LOG(HUD_LOG_LIFECYCLE, "%s: %d", #expr, (int)result_);   \
    } while (0)

VKAPI_ATTR VkResult VKAPI_CALL hud_CreateSwapchainKHR(VkDevice device,
                                                      const VkSwapchainCreateInfoKHR *pCreateInfo,
                                                      const VkAllocationCallbacks *pAllocator,
                                                      VkSwapchainKHR *pSwapchain);
VKAPI_ATTR void VKAPI_CALL hud_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                    const VkAllocationCallbacks *pAllocator);
VKAPI_ATTR VkResult VKAPI_CALL hud_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo);

#endif /* WINEPIPEWIRE_HUD_LAYER_HPP */
