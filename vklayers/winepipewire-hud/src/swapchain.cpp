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
 * Derived from MangoHud src/vulkan.cpp (Intel base plus MH changes), adapted to
 * this project's types.  This list tracks what this file actually contains and
 * grows as further functions land:
 *   overlay_CreateSwapchainKHR, for the create-info copy that ORs in
 *     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT and the create-then-track shape
 *   overlay_DestroySwapchainKHR, for the null-handle case and the
 *     tear-down-before-chain order
 *   overlay_QueuePresentKHR, including the three rules that make it safe: one
 *     present per swapchain, the application's wait semaphores passed only for
 *     i == 0, and the wait list replaced by the overlay semaphore only when a
 *     draw was actually produced.  Mesa gets the last two wrong.
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

VKAPI_ATTR VkResult VKAPI_CALL hud_CreateSwapchainKHR(VkDevice device,
                                                      const VkSwapchainCreateInfoKHR *pCreateInfo,
                                                      const VkAllocationCallbacks *pAllocator,
                                                      VkSwapchainKHR *pSwapchain)
{
    struct device_data *device_data = device_data_find(dispatch_key(device));
    VkSwapchainCreateInfoKHR create_info = *pCreateInfo;
    bool app_color_attachment;
    VkResult result;

    /* A device that never enabled VK_KHR_swapchain has no chain function here,
     * and vkGetInstanceProcAddr hands out swapchain entry points regardless of
     * what the device enabled, so this is reachable from a buggy caller. */
    if (!device_data || !device_data->vtable.CreateSwapchainKHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    /* Drawing into a swapchain image as a colour attachment is invalid unless
     * the swapchain was created with that usage, and applications that only
     * blit or resolve into it do not ask for it.  Mesa's overlay passes
     * pCreateInfo through unchanged and inherits the bug; MangoHud ORs the bit
     * in on a copy, which is what this does.  The copy matters: pCreateInfo
     * belongs to the caller and outlives this call. */
    app_color_attachment = (create_info.imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    create_info.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    result = device_data->vtable.CreateSwapchainKHR(device, &create_info, pAllocator, pSwapchain);
    if (result != VK_SUCCESS)
    {
        HUD_LOG(HUD_LOG_LIFECYCLE, "vkCreateSwapchainKHR failed with %d", (int)result);
        return result;
    }

    struct swapchain_data *swapchain_data =
        swapchain_data_create(*pSwapchain, device_data, pCreateInfo, app_color_attachment);

    if (!hud_setup_swapchain(swapchain_data, pCreateInfo))
    {
        HUD_LOG(HUD_LOG_LIFECYCLE, "swapchain %p renderer setup failed, nothing will be drawn "
                "on it", (void *)(uintptr_t)*pSwapchain);
        hud_shutdown_swapchain(swapchain_data);
    }

    HUD_LOG(HUD_LOG_LIFECYCLE,
            "swapchain %p created, %ux%u, %u images, format %d, colour space %d, "
            "colour attachment usage %s, overlay %s",
            (void *)(uintptr_t)*pSwapchain, swapchain_data->extent.width,
            swapchain_data->extent.height, swapchain_data->image_count, (int)swapchain_data->format,
            (int)swapchain_data->color_space, app_color_attachment ? "requested by the application"
                                                                  : "added by the layer",
            swapchain_data->render.valid ? "ready" : "off");
    return result;
}

VKAPI_ATTR void VKAPI_CALL hud_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                   const VkAllocationCallbacks *pAllocator)
{
    struct device_data *device_data = device_data_find(dispatch_key(device));

    /* Reachable only from a caller that never enabled VK_KHR_swapchain, and
     * then there is nothing to forward to.  Silence would hide the leak. */
    if (!device_data || !device_data->vtable.DestroySwapchainKHR)
    {
        HUD_LOG(HUD_LOG_LIFECYCLE, "no swapchain dispatch for device %p, swapchain %p not destroyed",
                (void *)device, (void *)(uintptr_t)swapchain);
        return;
    }

    if (swapchain != VK_NULL_HANDLE)
    {
        struct swapchain_data *swapchain_data = swapchain_data_find(swapchain);

        if (swapchain_data)
        {
            HUD_LOG(HUD_LOG_LIFECYCLE, "swapchain %p destroyed after %llu presents",
                    (void *)(uintptr_t)swapchain, (unsigned long long)swapchain_data->presents);
            /* Our resources go before the swapchain they were built from, and
             * the teardown waits for our last submit. */
            hud_shutdown_swapchain(swapchain_data);
        }
        swapchain_data_destroy(swapchain);
    }
    device_data->vtable.DestroySwapchainKHR(device, swapchain, pAllocator);
}

/* Ours: the same values the overlay draws, on stderr, for runs with no screen to
 * look at and for machine-checkable output. */
static void hud_present_log(struct swapchain_data *swapchain_data,
                            const struct pwhud_snapshot *snap, uint32_t image_index)
{
    if (hud_log_level() >= HUD_LOG_PRESENT)
    {
        hud_logf("present swapchain %p image %u count %llu",
                 (void *)(uintptr_t)swapchain_data->swapchain, image_index,
                 (unsigned long long)swapchain_data->presents);
        if (snap)
            hud_snapshot_log(&swapchain_data->snapshot);
        return;
    }
    if (hud_log_level() >= HUD_LOG_RATE)
    {
        uint64_t now = hud_mono_ns();
        uint64_t elapsed = now - swapchain_data->report_ns;

        if (elapsed < 1000000000ull)
            return;

        uint64_t frames = swapchain_data->presents - swapchain_data->report_presents;

        hud_logf("swapchain %p %llu presents in %.3f s (%.1f/s), %ux%u, %u images, %llu samples, "
                 "%llu torn A, %llu torn B, overlay %s",
                 (void *)(uintptr_t)swapchain_data->swapchain, (unsigned long long)frames,
                 (double)elapsed / 1e9, (double)frames * 1e9 / (double)elapsed,
                 swapchain_data->extent.width, swapchain_data->extent.height,
                 swapchain_data->image_count,
                 (unsigned long long)swapchain_data->snapshot.samples,
                 (unsigned long long)swapchain_data->snapshot.torn_a_total,
                 (unsigned long long)swapchain_data->snapshot.torn_b_total,
                 swapchain_data->render.valid ? "drawing" : "off");
        if (snap)
            hud_snapshot_log(&swapchain_data->snapshot);
        else
            hud_logf("snapshot: no mapping for this process yet");
        swapchain_data->report_ns = now;
        swapchain_data->report_presents = swapchain_data->presents;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL hud_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    struct device_data *device_data = device_data_find(dispatch_key(queue));
    struct hud_queue *present_queue = hud_queue_find(queue);
    const struct pwhud_snapshot *snap;
    VkResult result = VK_SUCCESS;
    uint32_t i;

    if (!device_data || !device_data->vtable.QueuePresentKHR)
    {
        HUD_LOG(HUD_LOG_LIFECYCLE, "no swapchain dispatch for the device behind queue %p, present "
                "cannot be forwarded", (void *)queue);
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    /* Mapped once for the process, and the mapping is immutable afterwards, so
     * only the per-swapchain copies below are touched from the present thread. */
    snap = hud_snapshot_map();

    /* One present per swapchain, because each needs its own overlay semaphore in
     * its own wait list. */
    for (i = 0; i < pPresentInfo->swapchainCount; i++)
    {
        struct swapchain_data *swapchain_data = swapchain_data_find(pPresentInfo->pSwapchains[i]);
        VkSwapchainKHR swapchain = pPresentInfo->pSwapchains[i];
        uint32_t image_index = pPresentInfo->pImageIndices[i];
        VkPresentInfoKHR present_info = *pPresentInfo;
        struct hud_draw *draw = nullptr;
        VkResult chain_result;

        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain;
        present_info.pImageIndices = &image_index;
        present_info.pResults = nullptr;

        if (swapchain_data)
        {
            swapchain_data->presents++;
            if (snap)
                hud_snapshot_sample(&swapchain_data->snapshot, snap);
            hud_present_log(swapchain_data, snap, image_index);

            /* The application's wait semaphores go to the first swapchain only.
             * Handing the same semaphores to every overlay submit would wait on
             * them more than once, which Mesa does and MangoHud fixed. */
            draw = hud_before_present(swapchain_data, present_queue,
                                      pPresentInfo->pWaitSemaphores,
                                      i == 0 ? pPresentInfo->waitSemaphoreCount : 0, image_index);
        }

        /* Only when a draw exists: the overlay submit already waited on the
         * application's semaphores, so the present waits on ours instead.  With
         * no draw the application's list has to stand, and Mesa's unconditional
         * replacement waits on an unsignalled semaphore. */
        if (draw)
        {
            present_info.pWaitSemaphores = &draw->semaphore;
            present_info.waitSemaphoreCount = 1;
        }

        chain_result = device_data->vtable.QueuePresentKHR(queue, &present_info);
        if (pPresentInfo->pResults)
            pPresentInfo->pResults[i] = chain_result;
        if (chain_result != VK_SUCCESS && result == VK_SUCCESS)
            result = chain_result;
    }

    return result;
}
