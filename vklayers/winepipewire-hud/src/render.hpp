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
 * Derived from MangoHud src/vulkan.cpp, adapted to this project's types.  This
 * list tracks what this file actually contains and grows as further functions
 * land:
 *   overlay_draw, as hud_draw
 *   queue_data, as hud_queue
 *   swapchain_data renderer fields, as hud_swapchain_render
 *   device_data renderer fields, as hud_device_render
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
#ifndef WINEPIPEWIRE_HUD_RENDER_HPP
#define WINEPIPEWIRE_HUD_RENDER_HPP

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "imgui.h"

struct device_data;
struct swapchain_data;

struct hud_queue
{
    struct device_data *device;
    VkQueue queue;
    VkQueueFlags flags;
    uint32_t family_index;
};

/* One per swapchain image, reused for the life of the swapchain.  The fence
 * bounds the number of these to the image count, at the cost of a blocking wait
 * when the application laps us. */
struct hud_draw
{
    VkCommandBuffer command_buffer;
    VkSemaphore cross_engine_semaphore;
    VkSemaphore semaphore;
    VkFence fence;

    VkBuffer vertex_buffer;
    VkDeviceMemory vertex_buffer_mem;
    VkDeviceSize vertex_buffer_size;

    VkBuffer index_buffer;
    VkDeviceMemory index_buffer_mem;
    VkDeviceSize index_buffer_size;
};

struct hud_device_render
{
    struct hud_queue *graphic_queue;
    std::vector<struct hud_queue *> queues;
};

struct hud_swapchain_render
{
    /* False when setup failed or has not run, in which case present passes
     * straight through and the application is unaffected. */
    bool valid;

    std::vector<VkImage> images;
    std::vector<VkImageView> image_views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<struct hud_draw *> draws;

    VkRenderPass render_pass;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_layout;
    VkSampler font_sampler;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkCommandPool command_pool;

    bool font_uploaded;
    VkImage font_image;
    VkImageView font_image_view;
    VkDeviceMemory font_mem;
    VkBuffer upload_font_buffer;
    VkDeviceMemory upload_font_buffer_mem;

    ImGuiContext *imgui;
    ImFontAtlas *font_atlas;
    uint64_t frame_ns;
};

void hud_device_map_queues(struct device_data *device_data, const VkDeviceCreateInfo *create_info);
void hud_device_unmap_queues(struct device_data *device_data);

bool hud_setup_swapchain(struct swapchain_data *swapchain_data,
                         const VkSwapchainCreateInfoKHR *create_info);
void hud_shutdown_swapchain(struct swapchain_data *swapchain_data);

/* Builds the frame and submits it, returning the draw whose semaphore the
 * present must wait on, or NULL when there is nothing to show. */
struct hud_draw *hud_before_present(struct swapchain_data *swapchain_data,
                                    struct hud_queue *present_queue,
                                    const VkSemaphore *wait_semaphores,
                                    unsigned n_wait_semaphores, unsigned image_index);

#endif /* WINEPIPEWIRE_HUD_RENDER_HPP */
