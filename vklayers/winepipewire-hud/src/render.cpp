/*
 * This file contains code derived from:
 *
 *   1) Mesa VK_LAYER_MESA_overlay / MangoHud src/vulkan.cpp ImGui draw path
 *      Copyright © 2019 Intel Corporation
 *
 *   2) MangoHud modifications to that path (font Alpha8 path, swapchain
 *      COLOR_ATTACHMENT usage, present/semaphore fixes, buffer atom size,
 *      per-image draw objects, dynamic font helpers, related shaders)
 *      Copyright (c) 2020 flightlessmango and MangoHud contributors
 *
 * Derived from MangoHud src/vulkan.cpp, adapted to this project's types.  What
 * this file actually contains:
 *   device_map_queues and new_queue_data, as hud_device_map_queues
 *   get_overlay_draw, as get_hud_draw
 *   vk_memory_type
 *   update_image_descriptor
 *   upload_image_data
 *   create_image
 *   create_image_with_desc
 *   ensure_swapchain_fonts, reduced to a one-shot upload because this overlay
 *     has no font hot reload, so MangoHud's check_fonts hash comparison and its
 *     font recreation path are not here
 *   CreateOrResizeBuffer
 *   render_swapchain_display, less its ImGui multi-context save and restore,
 *     its no_display parameter check and its HUD colour conversion
 *   setup_swapchain_data_pipeline
 *   setup_swapchain_data, less convert_colors_vk
 *   shutdown_swapchain_font
 *   shutdown_swapchain_data
 *   before_present entry structure, with our own frame build in place of
 *     snapshot_swapchain_frame and compute_swapchain_display
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include "layer.hpp"

#include <cstring>

/* GImGui is redirected here by hud_imconfig.h so that two swapchains presenting
 * from two threads cannot fight over the current context. */
thread_local ImGuiContext *hud_imgui_context;

/* Defined here rather than in a static library so the reference from the ImGui
 * objects always resolves: object files are pulled in whole, archives are not. */
void hud_imgui_assert(const char *expr, const char *file, int line)
{
    hud_logf("imgui assertion failed, continuing anyway: %s at %s:%d", expr, file, line);
}

/* The font atlas carries a VkDescriptorSet in ImTextureID.  A non-dispatchable
 * Vulkan handle stays 64 bits wide on i386, where VK_USE_64_BIT_PTR_DEFINES is 0
 * and the handle expands to uint64_t rather than shrinking to a pointer, so a
 * 32-bit ImTextureID would silently truncate it.  ImGui has defaulted
 * ImTextureID to ImU64 since 1.91.4 for this exact reason; checked here rather
 * than assumed to follow from the version pin. */
static_assert(sizeof(ImTextureID) >= sizeof(VkDescriptorSet),
              "ImTextureID cannot hold a VkDescriptorSet");

static const uint32_t overlay_vert_spv[] = {
#include "overlay.vert.spv.h"
};
static const uint32_t overlay_frag_spv[] = {
#include "overlay.frag.spv.h"
};

static uint32_t vk_memory_type(struct device_data *device_data, VkMemoryPropertyFlags properties,
                               uint32_t type_bits)
{
    VkPhysicalDeviceMemoryProperties prop;

    device_data->instance->vtable.GetPhysicalDeviceMemoryProperties(device_data->physical_device,
                                                                    &prop);
    for (uint32_t i = 0; i < prop.memoryTypeCount; i++)
        if ((prop.memoryTypes[i].propertyFlags & properties) == properties && type_bits & (1 << i))
            return i;
    return 0xffffffff;
}

static void update_image_descriptor(struct swapchain_data *swapchain_data, VkImageView image_view,
                                    VkDescriptorSet set)
{
    struct device_data *device_data = swapchain_data->device;
    VkDescriptorImageInfo desc_image[1] = {};
    VkWriteDescriptorSet write_desc[1] = {};

    desc_image[0].sampler = swapchain_data->render.font_sampler;
    desc_image[0].imageView = image_view;
    desc_image[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    write_desc[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write_desc[0].dstSet = set;
    write_desc[0].descriptorCount = 1;
    write_desc[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write_desc[0].pImageInfo = desc_image;
    device_data->vtable.UpdateDescriptorSets(device_data->device, 1, write_desc, 0, NULL);
}

/* Recorded into the overlay's own command buffer, inside the submit this layer
 * already owns.  That is the whole reason the stock ImGui backend is unusable
 * here: its font upload submits and wait-idles on an application queue. */
static void upload_image_data(struct device_data *device_data, VkCommandBuffer command_buffer,
                              void *pixels, VkDeviceSize upload_size, uint32_t width,
                              uint32_t height, VkBuffer &upload_buffer,
                              VkDeviceMemory &upload_buffer_mem, VkImage image)
{
    VkBufferCreateInfo buffer_info = {};
    VkMemoryRequirements upload_buffer_req;
    VkMemoryAllocateInfo upload_alloc_info = {};
    VkMappedMemoryRange range[1] = {};
    VkImageMemoryBarrier copy_barrier[1] = {};
    VkImageMemoryBarrier use_barrier[1] = {};
    VkBufferImageCopy region = {};
    char *map = NULL;

    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = upload_size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    HUD_VK(device_data->vtable.CreateBuffer(device_data->device, &buffer_info, NULL,
                                           &upload_buffer));
    device_data->vtable.GetBufferMemoryRequirements(device_data->device, upload_buffer,
                                                    &upload_buffer_req);
    upload_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    upload_alloc_info.allocationSize = upload_buffer_req.size;
    upload_alloc_info.memoryTypeIndex = vk_memory_type(device_data,
                                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                                       upload_buffer_req.memoryTypeBits);
    HUD_VK(device_data->vtable.AllocateMemory(device_data->device, &upload_alloc_info, NULL,
                                             &upload_buffer_mem));
    HUD_VK(device_data->vtable.BindBufferMemory(device_data->device, upload_buffer,
                                               upload_buffer_mem, 0));

    HUD_VK(device_data->vtable.MapMemory(device_data->device, upload_buffer_mem, 0, upload_size, 0,
                                        (void **)&map));
    memcpy(map, pixels, upload_size);
    range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range[0].memory = upload_buffer_mem;
    range[0].size = upload_size;
    HUD_VK(device_data->vtable.FlushMappedMemoryRanges(device_data->device, 1, range));
    device_data->vtable.UnmapMemory(device_data->device, upload_buffer_mem);

    copy_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    copy_barrier[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    copy_barrier[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    copy_barrier[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    copy_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    copy_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    copy_barrier[0].image = image;
    copy_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy_barrier[0].subresourceRange.levelCount = 1;
    copy_barrier[0].subresourceRange.layerCount = 1;
    device_data->vtable.CmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_HOST_BIT,
                                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                                           copy_barrier);

    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = width;
    region.imageExtent.height = height;
    region.imageExtent.depth = 1;
    device_data->vtable.CmdCopyBufferToImage(command_buffer, upload_buffer, image,
                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    use_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    use_barrier[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    use_barrier[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    use_barrier[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    use_barrier[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    use_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    use_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    use_barrier[0].image = image;
    use_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    use_barrier[0].subresourceRange.levelCount = 1;
    use_barrier[0].subresourceRange.layerCount = 1;
    device_data->vtable.CmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                                           NULL, 1, use_barrier);
}

static void create_image(struct swapchain_data *swapchain_data, VkDescriptorSet descriptor_set,
                         uint32_t width, uint32_t height, VkFormat format, VkImage &image,
                         VkDeviceMemory &image_mem, VkImageView &image_view)
{
    struct device_data *device_data = swapchain_data->device;
    VkImageCreateInfo image_info = {};
    VkMemoryRequirements image_req;
    VkMemoryAllocateInfo image_alloc_info = {};
    VkImageViewCreateInfo view_info = {};

    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent.width = width;
    image_info.extent.height = height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    HUD_VK(device_data->vtable.CreateImage(device_data->device, &image_info, NULL, &image));
    device_data->vtable.GetImageMemoryRequirements(device_data->device, image, &image_req);
    image_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    image_alloc_info.allocationSize = image_req.size;
    image_alloc_info.memoryTypeIndex = vk_memory_type(device_data,
                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                      image_req.memoryTypeBits);
    HUD_VK(device_data->vtable.AllocateMemory(device_data->device, &image_alloc_info, NULL,
                                             &image_mem));
    HUD_VK(device_data->vtable.BindImageMemory(device_data->device, image, image_mem, 0));

    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    HUD_VK(device_data->vtable.CreateImageView(device_data->device, &view_info, NULL, &image_view));

    update_image_descriptor(swapchain_data, image_view, descriptor_set);
}

static VkDescriptorSet create_image_with_desc(struct swapchain_data *swapchain_data, uint32_t width,
                                             uint32_t height, VkFormat format, VkImage &image,
                                             VkDeviceMemory &image_mem, VkImageView &image_view)
{
    struct device_data *device_data = swapchain_data->device;
    VkDescriptorSetAllocateInfo alloc_info = {};
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;

    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = swapchain_data->render.descriptor_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &swapchain_data->render.descriptor_layout;
    HUD_VK(device_data->vtable.AllocateDescriptorSets(device_data->device, &alloc_info,
                                                     &descriptor_set));

    create_image(swapchain_data, descriptor_set, width, height, format, image, image_mem,
                 image_view);
    return descriptor_set;
}

/* One shot: the atlas is built once per swapchain and never rebuilt, so there is
 * no font hash to compare and no recreation path. */
static void ensure_swapchain_fonts(struct swapchain_data *swapchain_data,
                                   VkCommandBuffer command_buffer)
{
    struct hud_swapchain_render *render = &swapchain_data->render;
    unsigned char *pixels;
    int width, height;

    if (render->font_uploaded)
        return;

    render->font_uploaded = true;
    render->font_atlas->GetTexDataAsAlpha8(&pixels, &width, &height);
    upload_image_data(swapchain_data->device, command_buffer, pixels,
                      (VkDeviceSize)width * height, (uint32_t)width, (uint32_t)height,
                      render->upload_font_buffer, render->upload_font_buffer_mem,
                      render->font_image);
}

static void create_or_resize_buffer(struct device_data *device_data, VkBuffer *buffer,
                                    VkDeviceMemory *buffer_memory, VkDeviceSize *buffer_size,
                                    size_t new_size, VkBufferUsageFlagBits usage)
{
    VkBufferCreateInfo buffer_info = {};
    VkMemoryRequirements req;
    VkMemoryAllocateInfo alloc_info = {};

    if (*buffer != VK_NULL_HANDLE)
        device_data->vtable.DestroyBuffer(device_data->device, *buffer, NULL);
    if (*buffer_memory)
        device_data->vtable.FreeMemory(device_data->device, *buffer_memory, NULL);

    /* The whole range is flushed with VK_WHOLE_SIZE, and a non-coherent
     * mapping's flush granularity is nonCoherentAtomSize, so the allocation is
     * rounded up to it. */
    if (device_data->properties.limits.nonCoherentAtomSize > 0)
    {
        VkDeviceSize atom_size = device_data->properties.limits.nonCoherentAtomSize - 1;

        new_size = (new_size + atom_size) & ~atom_size;
    }

    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = new_size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    HUD_VK(device_data->vtable.CreateBuffer(device_data->device, &buffer_info, NULL, buffer));

    device_data->vtable.GetBufferMemoryRequirements(device_data->device, *buffer, &req);
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = req.size;
    alloc_info.memoryTypeIndex = vk_memory_type(device_data, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                                req.memoryTypeBits);
    HUD_VK(device_data->vtable.AllocateMemory(device_data->device, &alloc_info, NULL,
                                             buffer_memory));
    HUD_VK(device_data->vtable.BindBufferMemory(device_data->device, *buffer, *buffer_memory, 0));
    *buffer_size = new_size;
}

/* One draw per swapchain image, reused.  The blocking wait bounds the number of
 * draw objects to the image count instead of growing a free list, which for an
 * overlay is the better trade: the application has already lapped us if the
 * fence is not ready. */
static struct hud_draw *get_hud_draw(struct swapchain_data *swapchain_data, unsigned image_idx)
{
    struct device_data *device_data = swapchain_data->device;
    struct hud_swapchain_render *render = &swapchain_data->render;
    VkCommandBufferAllocateInfo cmd_buffer_info = {};
    VkSemaphoreCreateInfo sem_info = {};
    VkFenceCreateInfo fence_info = {};
    struct hud_draw *draw;

    if (render->draws.size() < render->images.size())
        render->draws.resize(render->images.size(), nullptr);
    if (image_idx >= render->draws.size())
        return nullptr;
    if ((draw = render->draws[image_idx]))
    {
        HUD_VK(device_data->vtable.WaitForFences(device_data->device, 1, &draw->fence, VK_TRUE,
                                                ~0ull));
        HUD_VK(device_data->vtable.ResetFences(device_data->device, 1, &draw->fence));
        return draw;
    }

    draw = new hud_draw{};

    cmd_buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_buffer_info.commandPool = render->command_pool;
    cmd_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_buffer_info.commandBufferCount = 1;
    HUD_VK(device_data->vtable.AllocateCommandBuffers(device_data->device, &cmd_buffer_info,
                                                     &draw->command_buffer));
    /* A command buffer this layer allocated carries no loader dispatch data
     * until the loader is asked to install it. */
    HUD_VK(device_data->set_device_loader_data(device_data->device, draw->command_buffer));

    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    HUD_VK(device_data->vtable.CreateFence(device_data->device, &fence_info, NULL, &draw->fence));

    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    HUD_VK(device_data->vtable.CreateSemaphore(device_data->device, &sem_info, NULL,
                                              &draw->semaphore));
    HUD_VK(device_data->vtable.CreateSemaphore(device_data->device, &sem_info, NULL,
                                              &draw->cross_engine_semaphore));

    render->draws[image_idx] = draw;
    return draw;
}

static struct hud_draw *render_swapchain_display(struct swapchain_data *swapchain_data,
                                                 struct hud_queue *present_queue,
                                                 const VkSemaphore *wait_semaphores,
                                                 unsigned n_wait_semaphores, unsigned image_index)
{
    struct device_data *device_data = swapchain_data->device;
    struct hud_swapchain_render *render = &swapchain_data->render;
    ImDrawData *draw_data = ImGui::GetDrawData();
    VkRenderPassBeginInfo render_pass_info = {};
    VkCommandBufferBeginInfo buffer_begin_info = {};
    VkMappedMemoryRange range[2] = {};
    VkDescriptorSet desc_set[1];
    VkBuffer vertex_buffers[1];
    VkDeviceSize vertex_offset[1] = { 0 };
    ImDrawVert *vtx_dst = NULL;
    ImDrawIdx *idx_dst = NULL;
    VkImageMemoryBarrier imb = {};
    VkViewport viewport = {};
    struct hud_draw *draw;
    size_t vertex_size, index_size;
    float scale[2], translate[2];
    int vtx_offset = 0, idx_offset = 0;

    if (!draw_data || draw_data->TotalVtxCount == 0)
        return nullptr;
    if (image_index >= render->framebuffers.size())
        return nullptr;
    if (!(draw = get_hud_draw(swapchain_data, image_index)))
        return nullptr;

    device_data->vtable.ResetCommandBuffer(draw->command_buffer, 0);

    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_info.renderPass = render->render_pass;
    render_pass_info.framebuffer = render->framebuffers[image_index];
    render_pass_info.renderArea.extent.width = swapchain_data->extent.width;
    render_pass_info.renderArea.extent.height = swapchain_data->extent.height;

    buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    device_data->vtable.BeginCommandBuffer(draw->command_buffer, &buffer_begin_info);

    ensure_swapchain_fonts(swapchain_data, draw->command_buffer);

    /* Bounce the image about to be presented back to colour attachment layout so
     * it can be drawn on top of, and acquire it from the present queue family. */
    imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    imb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    imb.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    imb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    imb.image = render->images[image_index];
    imb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imb.subresourceRange.levelCount = 1;
    imb.subresourceRange.layerCount = 1;
    imb.srcQueueFamilyIndex = present_queue->family_index;
    imb.dstQueueFamilyIndex = device_data->render.graphic_queue->family_index;
    device_data->vtable.CmdPipelineBarrier(draw->command_buffer,
                                           VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                                           VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, nullptr, 0,
                                           nullptr, 1, &imb);

    device_data->vtable.CmdBeginRenderPass(draw->command_buffer, &render_pass_info,
                                           VK_SUBPASS_CONTENTS_INLINE);

    vertex_size = draw_data->TotalVtxCount * sizeof(ImDrawVert);
    index_size = draw_data->TotalIdxCount * sizeof(ImDrawIdx);
    if (draw->vertex_buffer_size < vertex_size)
        create_or_resize_buffer(device_data, &draw->vertex_buffer, &draw->vertex_buffer_mem,
                                &draw->vertex_buffer_size, vertex_size,
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (draw->index_buffer_size < index_size)
        create_or_resize_buffer(device_data, &draw->index_buffer, &draw->index_buffer_mem,
                                &draw->index_buffer_size, index_size,
                                VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    HUD_VK(device_data->vtable.MapMemory(device_data->device, draw->vertex_buffer_mem, 0,
                                        draw->vertex_buffer_size, 0, (void **)&vtx_dst));
    HUD_VK(device_data->vtable.MapMemory(device_data->device, draw->index_buffer_mem, 0,
                                        draw->index_buffer_size, 0, (void **)&idx_dst));
    for (int n = 0; n < draw_data->CmdListsCount; n++)
    {
        const ImDrawList *cmd_list = draw_data->CmdLists[n];

        memcpy(vtx_dst, cmd_list->VtxBuffer.Data,
               cmd_list->VtxBuffer.Size * sizeof(ImDrawVert));
        memcpy(idx_dst, cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx));
        vtx_dst += cmd_list->VtxBuffer.Size;
        idx_dst += cmd_list->IdxBuffer.Size;
    }
    range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range[0].memory = draw->vertex_buffer_mem;
    range[0].size = VK_WHOLE_SIZE;
    range[1].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range[1].memory = draw->index_buffer_mem;
    range[1].size = VK_WHOLE_SIZE;
    HUD_VK(device_data->vtable.FlushMappedMemoryRanges(device_data->device, 2, range));
    device_data->vtable.UnmapMemory(device_data->device, draw->vertex_buffer_mem);
    device_data->vtable.UnmapMemory(device_data->device, draw->index_buffer_mem);

    device_data->vtable.CmdBindPipeline(draw->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        render->pipeline);

    /* One font texture, so one descriptor set bound once rather than per draw
     * command.  ImTextureID is 64 bits wide at this ImGui pin even on i386,
     * which is what makes storing a VkDescriptorSet in it safe. */
    desc_set[0] = (VkDescriptorSet)render->font_atlas->TexID;
    device_data->vtable.CmdBindDescriptorSets(draw->command_buffer,
                                              VK_PIPELINE_BIND_POINT_GRAPHICS,
                                              render->pipeline_layout, 0, 1, desc_set, 0, NULL);

    vertex_buffers[0] = draw->vertex_buffer;
    device_data->vtable.CmdBindVertexBuffers(draw->command_buffer, 0, 1, vertex_buffers,
                                             vertex_offset);
    device_data->vtable.CmdBindIndexBuffer(draw->command_buffer, draw->index_buffer, 0,
                                           sizeof(ImDrawIdx) == 2 ? VK_INDEX_TYPE_UINT16
                                                                  : VK_INDEX_TYPE_UINT32);

    viewport.width = draw_data->DisplaySize.x;
    viewport.height = draw_data->DisplaySize.y;
    viewport.maxDepth = 1.0f;
    device_data->vtable.CmdSetViewport(draw->command_buffer, 0, 1, &viewport);

    scale[0] = 2.0f / draw_data->DisplaySize.x;
    scale[1] = 2.0f / draw_data->DisplaySize.y;
    translate[0] = -1.0f - draw_data->DisplayPos.x * scale[0];
    translate[1] = -1.0f - draw_data->DisplayPos.y * scale[1];
    device_data->vtable.CmdPushConstants(draw->command_buffer, render->pipeline_layout,
                                         VK_SHADER_STAGE_VERTEX_BIT, sizeof(float) * 0,
                                         sizeof(float) * 2, scale);
    device_data->vtable.CmdPushConstants(draw->command_buffer, render->pipeline_layout,
                                         VK_SHADER_STAGE_VERTEX_BIT, sizeof(float) * 2,
                                         sizeof(float) * 2, translate);

    for (int n = 0; n < draw_data->CmdListsCount; n++)
    {
        const ImDrawList *cmd_list = draw_data->CmdLists[n];

        for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
        {
            const ImDrawCmd *pcmd = &cmd_list->CmdBuffer[cmd_i];
            ImVec2 display_pos = draw_data->DisplayPos;
            VkRect2D scissor;

            scissor.offset.x = (int32_t)(pcmd->ClipRect.x - display_pos.x) > 0
                                   ? (int32_t)(pcmd->ClipRect.x - display_pos.x)
                                   : 0;
            scissor.offset.y = (int32_t)(pcmd->ClipRect.y - display_pos.y) > 0
                                   ? (int32_t)(pcmd->ClipRect.y - display_pos.y)
                                   : 0;
            scissor.extent.width = (uint32_t)(pcmd->ClipRect.z - pcmd->ClipRect.x);
            scissor.extent.height = (uint32_t)(pcmd->ClipRect.w - pcmd->ClipRect.y + 1);
            device_data->vtable.CmdSetScissor(draw->command_buffer, 0, 1, &scissor);

            device_data->vtable.CmdDrawIndexed(draw->command_buffer, pcmd->ElemCount, 1, idx_offset,
                                               vtx_offset, 0);
            idx_offset += pcmd->ElemCount;
        }
        vtx_offset += cmd_list->VtxBuffer.Size;
    }

    device_data->vtable.CmdEndRenderPass(draw->command_buffer);

    if (device_data->render.graphic_queue->family_index != present_queue->family_index)
    {
        /* Hand the image back to the present queue family.  The render pass has
         * already moved it to the present layout. */
        imb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        imb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        imb.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        imb.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        imb.srcQueueFamilyIndex = device_data->render.graphic_queue->family_index;
        imb.dstQueueFamilyIndex = present_queue->family_index;
        device_data->vtable.CmdPipelineBarrier(draw->command_buffer,
                                               VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                                               VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, nullptr,
                                               0, nullptr, 1, &imb);
    }

    device_data->vtable.EndCommandBuffer(draw->command_buffer);

    if (!n_wait_semaphores && device_data->render.graphic_queue->queue != present_queue->queue)
    {
        /* Presenting on a different queue than the overlay draws on, with no
         * application semaphore to order against, so an empty submit on the
         * present queue provides the edge to wait for. */
        VkPipelineStageFlags stages_wait = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit_info = {};

        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 0;
        submit_info.pWaitDstStageMask = &stages_wait;
        submit_info.waitSemaphoreCount = 0;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &draw->cross_engine_semaphore;
        device_data->vtable.QueueSubmit(present_queue->queue, 1, &submit_info, VK_NULL_HANDLE);

        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &draw->command_buffer;
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &draw->cross_engine_semaphore;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &draw->semaphore;
        device_data->vtable.QueueSubmit(device_data->render.graphic_queue->queue, 1, &submit_info,
                                        draw->fence);
    }
    else
    {
        std::vector<VkPipelineStageFlags> stages_wait(n_wait_semaphores,
                                                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        VkSubmitInfo submit_info = {};

        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &draw->command_buffer;
        submit_info.pWaitDstStageMask = stages_wait.data();
        submit_info.waitSemaphoreCount = n_wait_semaphores;
        submit_info.pWaitSemaphores = wait_semaphores;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &draw->semaphore;
        device_data->vtable.QueueSubmit(device_data->render.graphic_queue->queue, 1, &submit_info,
                                        draw->fence);
    }

    return draw;
}

static bool setup_swapchain_pipeline(struct swapchain_data *swapchain_data)
{
    struct device_data *device_data = swapchain_data->device;
    struct hud_swapchain_render *render = &swapchain_data->render;
    VkShaderModuleCreateInfo vert_info = {};
    VkShaderModuleCreateInfo frag_info = {};
    VkShaderModule vert_module, frag_module;
    VkSamplerCreateInfo sampler_info = {};
    VkDescriptorPoolSize sampler_pool_size = {};
    VkDescriptorPoolCreateInfo desc_pool_info = {};
    VkSampler sampler[1];
    VkDescriptorSetLayoutBinding binding[1] = {};
    VkDescriptorSetLayoutCreateInfo set_layout_info = {};
    VkPushConstantRange push_constants[1] = {};
    VkPipelineLayoutCreateInfo layout_info = {};
    VkPipelineShaderStageCreateInfo stage[2] = {};
    VkVertexInputBindingDescription binding_desc[1] = {};
    VkVertexInputAttributeDescription attribute_desc[3] = {};
    VkPipelineVertexInputStateCreateInfo vertex_info = {};
    VkPipelineInputAssemblyStateCreateInfo ia_info = {};
    VkPipelineViewportStateCreateInfo viewport_info = {};
    VkPipelineRasterizationStateCreateInfo raster_info = {};
    VkPipelineMultisampleStateCreateInfo ms_info = {};
    VkPipelineColorBlendAttachmentState color_attachment[1] = {};
    VkPipelineDepthStencilStateCreateInfo depth_info = {};
    VkPipelineColorBlendStateCreateInfo blend_info = {};
    VkDynamicState dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic_state = {};
    VkGraphicsPipelineCreateInfo info = {};
    VkDescriptorSet font_desc_set;
    unsigned char *pixels;
    int font_width, font_height;
    VkResult result;

    vert_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vert_info.codeSize = sizeof(overlay_vert_spv);
    vert_info.pCode = overlay_vert_spv;
    HUD_VK(device_data->vtable.CreateShaderModule(device_data->device, &vert_info, NULL,
                                                 &vert_module));
    frag_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    frag_info.codeSize = sizeof(overlay_frag_spv);
    frag_info.pCode = overlay_frag_spv;
    HUD_VK(device_data->vtable.CreateShaderModule(device_data->device, &frag_info, NULL,
                                                 &frag_module));

    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.minLod = -1000;
    sampler_info.maxLod = 1000;
    sampler_info.maxAnisotropy = 1.0f;
    HUD_VK(device_data->vtable.CreateSampler(device_data->device, &sampler_info, NULL,
                                            &render->font_sampler));

    sampler_pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampler_pool_size.descriptorCount = 1;
    desc_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    desc_pool_info.maxSets = 1;
    desc_pool_info.poolSizeCount = 1;
    desc_pool_info.pPoolSizes = &sampler_pool_size;
    HUD_VK(device_data->vtable.CreateDescriptorPool(device_data->device, &desc_pool_info, NULL,
                                                   &render->descriptor_pool));

    sampler[0] = render->font_sampler;
    binding[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding[0].descriptorCount = 1;
    binding[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binding[0].pImmutableSamplers = sampler;
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 1;
    set_layout_info.pBindings = binding;
    HUD_VK(device_data->vtable.CreateDescriptorSetLayout(device_data->device, &set_layout_info,
                                                        NULL, &render->descriptor_layout));

    push_constants[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_constants[0].offset = sizeof(float) * 0;
    push_constants[0].size = sizeof(float) * 4;
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &render->descriptor_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = push_constants;
    HUD_VK(device_data->vtable.CreatePipelineLayout(device_data->device, &layout_info, NULL,
                                                   &render->pipeline_layout));

    stage[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage[0].module = vert_module;
    stage[0].pName = "main";
    stage[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stage[1].module = frag_module;
    stage[1].pName = "main";

    binding_desc[0].stride = sizeof(ImDrawVert);
    binding_desc[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    attribute_desc[0].location = 0;
    attribute_desc[0].binding = binding_desc[0].binding;
    attribute_desc[0].format = VK_FORMAT_R32G32_SFLOAT;
    attribute_desc[0].offset = offsetof(ImDrawVert, pos);
    attribute_desc[1].location = 1;
    attribute_desc[1].binding = binding_desc[0].binding;
    attribute_desc[1].format = VK_FORMAT_R32G32_SFLOAT;
    attribute_desc[1].offset = offsetof(ImDrawVert, uv);
    attribute_desc[2].location = 2;
    attribute_desc[2].binding = binding_desc[0].binding;
    attribute_desc[2].format = VK_FORMAT_R8G8B8A8_UNORM;
    attribute_desc[2].offset = offsetof(ImDrawVert, col);

    vertex_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_info.vertexBindingDescriptionCount = 1;
    vertex_info.pVertexBindingDescriptions = binding_desc;
    vertex_info.vertexAttributeDescriptionCount = 3;
    vertex_info.pVertexAttributeDescriptions = attribute_desc;

    ia_info.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia_info.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    viewport_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_info.viewportCount = 1;
    viewport_info.scissorCount = 1;

    raster_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster_info.polygonMode = VK_POLYGON_MODE_FILL;
    raster_info.cullMode = VK_CULL_MODE_NONE;
    raster_info.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster_info.lineWidth = 1.0f;

    ms_info.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms_info.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    color_attachment[0].blendEnable = VK_TRUE;
    color_attachment[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    color_attachment[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    color_attachment[0].colorBlendOp = VK_BLEND_OP_ADD;
    color_attachment[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    color_attachment[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    color_attachment[0].alphaBlendOp = VK_BLEND_OP_ADD;
    color_attachment[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    depth_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    blend_info.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend_info.attachmentCount = 1;
    blend_info.pAttachments = color_attachment;

    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;

    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stage;
    info.pVertexInputState = &vertex_info;
    info.pInputAssemblyState = &ia_info;
    info.pViewportState = &viewport_info;
    info.pRasterizationState = &raster_info;
    info.pMultisampleState = &ms_info;
    info.pDepthStencilState = &depth_info;
    info.pColorBlendState = &blend_info;
    info.pDynamicState = &dynamic_state;
    info.layout = render->pipeline_layout;
    info.renderPass = render->render_pass;
    result = device_data->vtable.CreateGraphicsPipelines(device_data->device, VK_NULL_HANDLE, 1,
                                                        &info, NULL, &render->pipeline);
    HUD_VK(result);

    device_data->vtable.DestroyShaderModule(device_data->device, vert_module, NULL);
    device_data->vtable.DestroyShaderModule(device_data->device, frag_module, NULL);

    if (result != VK_SUCCESS)
        return false;

    /* Alpha8 to match the fragment shader's single-channel sample, and the
     * descriptor set is what the atlas carries as its texture id. */
    render->font_atlas->GetTexDataAsAlpha8(&pixels, &font_width, &font_height);
    font_desc_set = create_image_with_desc(swapchain_data, (uint32_t)font_width,
                                          (uint32_t)font_height, VK_FORMAT_R8_UNORM,
                                          render->font_image, render->font_mem,
                                          render->font_image_view);
    if (!font_desc_set)
        return false;
    render->font_atlas->SetTexID((ImTextureID)font_desc_set);
    return true;
}

bool hud_setup_swapchain(struct swapchain_data *swapchain_data,
                         const VkSwapchainCreateInfoKHR *create_info)
{
    struct device_data *device_data = swapchain_data->device;
    struct hud_swapchain_render *render = &swapchain_data->render;
    VkAttachmentDescription attachment_desc = {};
    VkAttachmentReference color_attachment = {};
    VkSubpassDescription subpass = {};
    VkSubpassDependency dependency = {};
    VkRenderPassCreateInfo render_pass_info = {};
    VkImageViewCreateInfo view_info = {};
    VkImageView attachment[1];
    VkFramebufferCreateInfo fb_info = {};
    VkCommandPoolCreateInfo cmd_buffer_pool_info = {};
    ImGuiContext *saved = ImGui::GetCurrentContext();
    uint32_t n_images = 0;

    if (!device_data->render.graphic_queue)
    {
        HUD_LOG(HUD_LOG_LIFECYCLE, "device %p has no graphics queue, nothing will be drawn",
                (void *)device_data->device);
        return false;
    }

    render->font_atlas = IM_NEW(ImFontAtlas);
    render->imgui = ImGui::CreateContext(render->font_atlas);
    ImGui::SetCurrentContext(render->imgui);
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().LogFilename = nullptr;
    ImGui::GetIO().DisplaySize = ImVec2((float)swapchain_data->extent.width,
                                        (float)swapchain_data->extent.height);
    ImGui::StyleColorsDark();

    /* The baked default font is ProggyClean at 13 px, so an unscaled panel keeps
     * the same pixel size at every resolution and shrinks by half in apparent
     * size on the way from 1080p to 4K.  An integer factor keeps the font on its
     * pixel grid, and the atlas is per swapchain and rebuilt whenever the
     * application recreates one, so a resolution change needs no font reload.
     * Ours, not lifted: MangoHud takes its size from a config file instead. */
    if (unsigned scale = swapchain_data->extent.height / 1080)
    {
        ImFontConfig font_cfg;

        if (scale > 4)
            scale = 4;
        font_cfg.SizePixels = 13.0f * (float)scale;
        render->font_atlas->AddFontDefault(&font_cfg);
        ImGui::GetStyle().ScaleAllSizes((float)scale);
        HUD_LOG(HUD_LOG_LIFECYCLE, "swapchain is %u px tall, drawing at %ux with a %.0f px font",
                swapchain_data->extent.height, scale, (double)font_cfg.SizePixels);
    }
    ImGui::SetCurrentContext(saved);

    /* LOAD, not CLEAR: the application's frame is already in the image and the
     * overlay is composited on top of it.  The final layout hands the image
     * straight back to the presentation engine. */
    attachment_desc.format = create_info->imageFormat;
    attachment_desc.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment_desc.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment_desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment_desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment_desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment_desc.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment_desc.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    color_attachment.attachment = 0;
    color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_attachment;
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &attachment_desc;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = 1;
    render_pass_info.pDependencies = &dependency;
    if (device_data->vtable.CreateRenderPass(device_data->device, &render_pass_info, NULL,
                                             &render->render_pass) != VK_SUCCESS)
        return false;

    if (!setup_swapchain_pipeline(swapchain_data))
        return false;

    HUD_VK(device_data->vtable.GetSwapchainImagesKHR(device_data->device,
                                                    swapchain_data->swapchain, &n_images, NULL));
    if (!n_images)
        return false;
    render->images.resize(n_images);
    render->image_views.resize(n_images);
    render->framebuffers.resize(n_images);
    HUD_VK(device_data->vtable.GetSwapchainImagesKHR(device_data->device,
                                                    swapchain_data->swapchain, &n_images,
                                                    render->images.data()));

    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = create_info->imageFormat;
    view_info.components.r = VK_COMPONENT_SWIZZLE_R;
    view_info.components.g = VK_COMPONENT_SWIZZLE_G;
    view_info.components.b = VK_COMPONENT_SWIZZLE_B;
    view_info.components.a = VK_COMPONENT_SWIZZLE_A;
    view_info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    for (uint32_t i = 0; i < n_images; i++)
    {
        view_info.image = render->images[i];
        HUD_VK(device_data->vtable.CreateImageView(device_data->device, &view_info, NULL,
                                                  &render->image_views[i]));
    }

    fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_info.renderPass = render->render_pass;
    fb_info.attachmentCount = 1;
    fb_info.pAttachments = attachment;
    fb_info.width = swapchain_data->extent.width;
    fb_info.height = swapchain_data->extent.height;
    fb_info.layers = 1;
    for (uint32_t i = 0; i < n_images; i++)
    {
        attachment[0] = render->image_views[i];
        HUD_VK(device_data->vtable.CreateFramebuffer(device_data->device, &fb_info, NULL,
                                                    &render->framebuffers[i]));
    }

    cmd_buffer_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmd_buffer_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cmd_buffer_pool_info.queueFamilyIndex = device_data->render.graphic_queue->family_index;
    if (device_data->vtable.CreateCommandPool(device_data->device, &cmd_buffer_pool_info, NULL,
                                              &render->command_pool) != VK_SUCCESS)
        return false;

    render->valid = true;
    return true;
}

static void shutdown_swapchain_font(struct swapchain_data *swapchain_data)
{
    struct device_data *device_data = swapchain_data->device;
    struct hud_swapchain_render *render = &swapchain_data->render;

    device_data->vtable.DestroyImageView(device_data->device, render->font_image_view, NULL);
    device_data->vtable.DestroyImage(device_data->device, render->font_image, NULL);
    device_data->vtable.FreeMemory(device_data->device, render->font_mem, NULL);
    device_data->vtable.DestroyBuffer(device_data->device, render->upload_font_buffer, NULL);
    device_data->vtable.FreeMemory(device_data->device, render->upload_font_buffer_mem, NULL);
}

void hud_shutdown_swapchain(struct swapchain_data *swapchain_data)
{
    struct device_data *device_data = swapchain_data->device;
    struct hud_swapchain_render *render = &swapchain_data->render;

    /* The application may destroy the swapchain while our last overlay submit is
     * still in flight, and its fences and command buffers are about to go. */
    device_data->vtable.DeviceWaitIdle(device_data->device);

    for (struct hud_draw *draw : render->draws)
    {
        if (!draw)
            continue;
        device_data->vtable.FreeCommandBuffers(device_data->device, render->command_pool, 1,
                                               &draw->command_buffer);
        device_data->vtable.DestroySemaphore(device_data->device, draw->cross_engine_semaphore,
                                             NULL);
        device_data->vtable.DestroySemaphore(device_data->device, draw->semaphore, NULL);
        device_data->vtable.DestroyFence(device_data->device, draw->fence, NULL);
        device_data->vtable.DestroyBuffer(device_data->device, draw->vertex_buffer, NULL);
        device_data->vtable.DestroyBuffer(device_data->device, draw->index_buffer, NULL);
        device_data->vtable.FreeMemory(device_data->device, draw->vertex_buffer_mem, NULL);
        device_data->vtable.FreeMemory(device_data->device, draw->index_buffer_mem, NULL);
        delete draw;
    }
    render->draws.clear();

    for (size_t i = 0; i < render->images.size(); i++)
    {
        device_data->vtable.DestroyImageView(device_data->device, render->image_views[i], NULL);
        device_data->vtable.DestroyFramebuffer(device_data->device, render->framebuffers[i], NULL);
    }

    device_data->vtable.DestroyRenderPass(device_data->device, render->render_pass, NULL);
    device_data->vtable.DestroyCommandPool(device_data->device, render->command_pool, NULL);
    device_data->vtable.DestroyPipeline(device_data->device, render->pipeline, NULL);
    device_data->vtable.DestroyPipelineLayout(device_data->device, render->pipeline_layout, NULL);
    device_data->vtable.DestroyDescriptorPool(device_data->device, render->descriptor_pool, NULL);
    device_data->vtable.DestroyDescriptorSetLayout(device_data->device, render->descriptor_layout,
                                                   NULL);
    device_data->vtable.DestroySampler(device_data->device, render->font_sampler, NULL);
    shutdown_swapchain_font(swapchain_data);

    if (render->imgui)
    {
        ImGuiContext *saved = ImGui::GetCurrentContext();

        /* DestroyContext frees the atlas only when the context owns it, and this
         * one does not: the atlas was handed to CreateContext. */
        ImGui::DestroyContext(render->imgui);
        ImGui::SetCurrentContext(saved == render->imgui ? nullptr : saved);
        render->imgui = nullptr;
    }
    if (render->font_atlas)
    {
        IM_DELETE(render->font_atlas);
        render->font_atlas = nullptr;
    }
    render->valid = false;
}

struct hud_draw *hud_before_present(struct swapchain_data *swapchain_data,
                                    struct hud_queue *present_queue,
                                    const VkSemaphore *wait_semaphores,
                                    unsigned n_wait_semaphores, unsigned image_index)
{
    struct hud_swapchain_render *render = &swapchain_data->render;
    ImGuiContext *saved = ImGui::GetCurrentContext();
    struct hud_draw *draw;
    uint64_t now = hud_mono_ns();

    if (!render->valid || !present_queue)
        return nullptr;

    ImGui::SetCurrentContext(render->imgui);
    ImGui::GetIO().DeltaTime = render->frame_ns
                                   ? (float)(now - render->frame_ns) / 1e9f
                                   : 1.0f / 60.0f;
    if (ImGui::GetIO().DeltaTime <= 0.0f)
        ImGui::GetIO().DeltaTime = 1.0f / 1000.0f;
    render->frame_ns = now;

    hud_build_frame(swapchain_data);
    draw = render_swapchain_display(swapchain_data, present_queue, wait_semaphores,
                                    n_wait_semaphores, image_index);
    ImGui::SetCurrentContext(saved);
    return draw;
}

void hud_device_map_queues(struct device_data *device_data, const VkDeviceCreateInfo *create_info)
{
    struct instance_data *instance_data = device_data->instance;
    uint32_t n_family_props = 0;

    instance_data->vtable.GetPhysicalDeviceQueueFamilyProperties(device_data->physical_device,
                                                                 &n_family_props, NULL);
    std::vector<VkQueueFamilyProperties> family_props(n_family_props);
    instance_data->vtable.GetPhysicalDeviceQueueFamilyProperties(device_data->physical_device,
                                                                 &n_family_props,
                                                                 family_props.data());

    for (uint32_t i = 0; i < create_info->queueCreateInfoCount; i++)
    {
        uint32_t family_index = create_info->pQueueCreateInfos[i].queueFamilyIndex;

        if (family_index >= n_family_props)
            continue;
        for (uint32_t j = 0; j < create_info->pQueueCreateInfos[i].queueCount; j++)
        {
            struct hud_queue *queue_data;
            VkQueue queue;

            device_data->vtable.GetDeviceQueue(device_data->device, family_index, j, &queue);
            HUD_VK(device_data->set_device_loader_data(device_data->device, queue));

            queue_data = hud_queue_register(device_data, queue,
                                            family_props[family_index].queueFlags, family_index);
            device_data->render.queues.push_back(queue_data);
            if (!device_data->render.graphic_queue &&
                (queue_data->flags & VK_QUEUE_GRAPHICS_BIT))
                device_data->render.graphic_queue = queue_data;
        }
    }
}

void hud_device_unmap_queues(struct device_data *device_data)
{
    for (struct hud_queue *queue_data : device_data->render.queues)
    {
        hud_queue_unregister(queue_data->queue);
        delete queue_data;
    }
    device_data->render.queues.clear();
    device_data->render.graphic_queue = nullptr;
}
