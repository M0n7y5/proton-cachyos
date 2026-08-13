/*
 * Presents a few frames through VK_EXT_headless_surface, requesting only
 * VK_IMAGE_USAGE_TRANSFER_DST_BIT on the swapchain and never rendering into it
 * as a colour attachment.  That is the case the layer's create-info copy exists
 * for, and the one vkcube cannot exercise because it asks for the attachment
 * usage itself.  Needs no display, so it also runs where vkcube cannot.
 *
 *   present_smoke [frames] [publish_seconds]
 *
 * With publish_seconds, a thread stands in for the driver and publishes a
 * snapshot for this process for that long while the presenting continues for two
 * seconds more.  That drives the layer's production discovery path, which is
 * this process's own pid with no override, and then its idle path once the
 * publisher stops.  A game has the driver and the layer in one process, which is
 * the arrangement being reproduced here without a Wine tree.
 *
 * Do not combine publish_seconds with WINEPIPEWIRE_HUD_OVERLAY_PID.  That
 * variable retargets the layer at another process's snapshot, and the publisher
 * below builds its path with the same hud_snapshot_path() the layer uses, so it
 * resolves to the same file: the harness then overwrites the snapshot it was
 * pointed at and the frame shows this publisher's values under the other
 * process's pid.  It looks like the layer read the wrong file.  To watch a real
 * driver in another process, use frame-count mode, which starts no publisher.
 *
 * Exit 0 presented, 77 skipped (no headless surface support), 1 failed.
 *
 * SPDX-License-Identifier: MIT
 */
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <vulkan/vulkan.h>

#include "../src/log.hpp"
#include "hud_publisher.hpp"

#define CHECK(expr)                                                              \
    do {                                                                         \
        VkResult result_ = (expr);                                               \
                                                                                 \
        if (result_ != VK_SUCCESS)                                               \
        {                                                                        \
            fprintf(stderr, "present_smoke: %s: %d\n", #expr, (int)result_);     \
            return 1;                                                            \
        }                                                                        \
    } while (0)

static bool has_instance_extension(const char *name)
{
    std::vector<VkExtensionProperties> extensions;
    uint32_t count = 0;

    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS)
        return false;
    extensions.resize(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()) != VK_SUCCESS)
        return false;
    for (const VkExtensionProperties &extension : extensions)
        if (!strcmp(extension.extensionName, name))
            return true;
    return false;
}

/* Stands in for the driver's period timer thread: republishes section A at 100
 * Hz and section B at 10 Hz with values that move, so a consumer that froze or
 * read the wrong field is visible in the log rather than plausible. */
static void publish_loop(struct hud_publisher *pub, std::atomic<bool> *run)
{
    /* The real driver cannot measure the graph's DSP load and says so with the
     * flag, which is the default here too.  Setting this publishes a load with
     * the flag clear instead, so both renderings can be looked at. */
    const char *dsp_env = getenv("WINEPIPEWIRE_HUD_SMOKE_DSP");
    const float dsp_load = dsp_env ? (float)atof(dsp_env) : 0.0f;
    /* Section B has three states and all three must be reachable here: never
     * published, published with an empty bed, and published with levels.  A mask
     * of 0 is the middle one; the publisher thread not running at all is the
     * first. */
    const char *mask_env = getenv("WINEPIPEWIRE_HUD_SMOKE_BEDMASK");
    const uint32_t bed_mask = mask_env ? (uint32_t)strtoul(mask_env, nullptr, 0)
                                       : (1u << 0) | (1u << 1);
    /* Bed channels pinned to the producer's floor, and to a NaN.  A floor reading
     * beside a live one is the pair the display has to keep distinguishable from
     * absence, and a NaN is what the spatial publisher emits for a title feeding
     * NaN audio, which is a render hazard rather than a wrong number. */
    const char *floor_env = getenv("WINEPIPEWIRE_HUD_SMOKE_BEDFLOOR");
    const uint32_t bed_floor = floor_env ? (uint32_t)strtoul(floor_env, nullptr, 0) : 0;
    const char *nan_env = getenv("WINEPIPEWIRE_HUD_SMOKE_BEDNAN");
    const uint32_t bed_nan = nan_env ? (uint32_t)strtoul(nan_env, nullptr, 0) : 0;
    /* The output meter's three reachable states, which are the ones that must
     * never look alike: a real level, nothing held at the publish point, and a
     * stream whose format carries no meter.  Asking for more channels than the
     * snapshot meters clamps and reports truncation, the same way the driver
     * does on a wider endpoint. */
    const char *out_env = getenv("WINEPIPEWIRE_HUD_SMOKE_OUT");
    uint32_t out_channels = out_env ? (uint32_t)strtoul(out_env, nullptr, 0) : 2;
    const uint32_t out_flags = out_channels > PWHUD_OUT_MAX ? PWHUD_F_OUT_TRUNCATED : 0;
    const char *held_env = getenv("WINEPIPEWIRE_HUD_SMOKE_HELD");
    const uint32_t no_meter = getenv("WINEPIPEWIRE_HUD_SMOKE_NOMETER") ? PWHUD_F_OUT_NO_METER : 0;
    /* Every value below normally moves with the tick, which is what proves a
     * consumer is reading rather than remembering.  Freezing them makes a
     * rendered frame reproducible, which is what lets two architectures be
     * compared byte for byte instead of by eye. */
    const bool statics = getenv("WINEPIPEWIRE_HUD_SMOKE_STATIC") != nullptr;
    /* Tick at which drv_underruns drops from 249 to 5, or 0 to leave it alone. */
    const char *understep_env = getenv("WINEPIPEWIRE_HUD_SMOKE_UNDERSTEP");
    const unsigned understep = understep_env ? (unsigned)strtoul(understep_env, nullptr, 10) : 0;

    if (out_channels > PWHUD_OUT_MAX)
        out_channels = PWHUD_OUT_MAX;

    for (unsigned tick = 0; run->load(); tick++)
    {
        struct pwhud_snapshot *snap = pub->snap;
        struct timespec ts;
        unsigned step = statics ? 0 : tick;
        uint64_t held = held_env ? strtoull(held_env, nullptr, 0) : 4096 + 2048 * (step % 8);

        clock_gettime(CLOCK_MONOTONIC, &ts);
        pub->a_begin();
        snap->clock_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        pwhud_flags_publish(snap, PWHUD_F_MASK_A,
                            PWHUD_F_GRID_VALID | (dsp_env ? 0 : PWHUD_F_NO_DSP_LOAD) |
                                out_flags | no_meter);
        snap->pw_quantum = 512;
        snap->pw_rate = 48000;
        /* The graph driver node's, not ours, and deliberately much smaller than
         * the ring underrun count so a display that confused them would show it. */
        snap->pw_xruns = 2;
        snap->pw_dsp_load = dsp_load;
        snap->pw_stream_count = 1;
        snap->drv_dispatch = PWHUD_DISPATCH_DATA;
        snap->drv_period_usec = 10666;
        snap->drv_held_bytes = held;
        snap->drv_ring_bytes = 24576;
        snap->drv_period_bytes = 4096;
        snap->drv_phase_adjust_us = -60 + (int64_t)(step % 30);
        /* A total that goes down, which is not a fabricated case: the driver sums
         * these over its live streams, so an orderly stream teardown removes a
         * released stream's history from the total.  A real session stepped 9 to 5
         * that way and the overlay painted it as a fault happening now. */
        snap->drv_underruns = understep && tick >= understep ? 5 : 249;
        snap->out_channels = out_channels;
        for (unsigned i = 0; i < PWHUD_OUT_MAX; i++)
            snap->out_peak_db[i] = i < out_channels
                                       ? -6.0f - 3.0f * (float)i - (float)(step % 12)
                                       : PWHUD_DB_FLOOR;
        pub->a_end();

        if (!(tick % 10))
        {
            pub->b_begin();
            snap->sp_hrtf = 1;
            snap->sp_bed_virtualized = 1;
            snap->sp_bed_mask = bed_mask;
            snap->sp_dyn_live = step / 10 % 5;
            snap->sp_dyn_max = 128;
            for (unsigned i = 0; i < PWHUD_BED_MAX; i++)
            {
                if (!(bed_mask & (1u << i)))
                    snap->sp_bed_db[i] = PWHUD_DB_FLOOR;
                else if (bed_nan & (1u << i))
                    snap->sp_bed_db[i] = NAN;
                else if (bed_floor & (1u << i))
                    snap->sp_bed_db[i] = PWHUD_DB_FLOOR;
                else
                    snap->sp_bed_db[i] = -10.0f - 2.0f * (float)i - (float)(step % 6);
            }
            pwhud_flags_publish(snap, PWHUD_F_MASK_B, 0);
            pub->b_end();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

/* Reads the presented image back and asserts that the overlay drew where it is
 * supposed to and nowhere else.  The reference value is taken from the far
 * corner, which the window cannot reach, so the check calibrates itself against
 * whatever byte order and encoding the surface format uses.
 *
 * Reading a presented image is only sound because this is the headless WSI,
 * which does not touch the image between present and the next acquire.  Against
 * a real presentation engine the contents would be undefined. */
struct readback_params
{
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    VkCommandBuffer cmd;
    VkImage image;
    VkExtent2D extent;
    /* With the gate unset the layer must leave the frame alone, which is as much
     * a requirement as drawing when it is set. */
    bool expect_overlay;
};

static uint32_t memory_type(VkPhysicalDevice physical_device, VkMemoryPropertyFlags properties,
                            uint32_t type_bits)
{
    VkPhysicalDeviceMemoryProperties prop;

    vkGetPhysicalDeviceMemoryProperties(physical_device, &prop);
    for (uint32_t i = 0; i < prop.memoryTypeCount; i++)
        if ((prop.memoryTypes[i].propertyFlags & properties) == properties && type_bits & (1u << i))
            return i;
    return 0xffffffffu;
}

static int verify_overlay_pixels(const struct readback_params *p)
{
    const VkDeviceSize size = (VkDeviceSize)p->extent.width * p->extent.height * 4;
    VkBufferCreateInfo buffer_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements req;
    VkMemoryAllocateInfo alloc_info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkCommandBufferBeginInfo begin_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkBufferImageCopy region = {};
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkBuffer buffer;
    VkDeviceMemory memory;
    const uint8_t *pixels;
    void *map = nullptr;
    uint32_t reference;
    uint32_t changed = 0, bottom_changed = 0;
    uint32_t min_x = p->extent.width, min_y = p->extent.height, max_x = 0, max_y = 0;
    int status = 0;

    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CHECK(vkCreateBuffer(p->device, &buffer_info, nullptr, &buffer));
    vkGetBufferMemoryRequirements(p->device, buffer, &req);
    alloc_info.allocationSize = req.size;
    alloc_info.memoryTypeIndex = memory_type(p->physical_device,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                             req.memoryTypeBits);
    CHECK(vkAllocateMemory(p->device, &alloc_info, nullptr, &memory));
    CHECK(vkBindBufferMemory(p->device, buffer, memory, 0));

    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(vkResetCommandBuffer(p->cmd, 0));
    CHECK(vkBeginCommandBuffer(p->cmd, &begin_info));

    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = p->image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(p->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { p->extent.width, p->extent.height, 1 };
    vkCmdCopyImageToBuffer(p->cmd, p->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
                           &region);
    CHECK(vkEndCommandBuffer(p->cmd));

    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &p->cmd;
    CHECK(vkQueueSubmit(p->queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(p->queue));
    CHECK(vkMapMemory(p->device, memory, 0, size, 0, &map));

    pixels = (const uint8_t *)map;
    memcpy(&reference, pixels + ((size_t)(p->extent.height - 1) * p->extent.width +
                                 (p->extent.width - 1)) * 4, 4);

    for (uint32_t y = 0; y < p->extent.height; y++)
    {
        for (uint32_t x = 0; x < p->extent.width; x++)
        {
            uint32_t value;

            memcpy(&value, pixels + ((size_t)y * p->extent.width + x) * 4, 4);
            if (value == reference)
                continue;
            changed++;
            if (x < min_x)
                min_x = x;
            if (y < min_y)
                min_y = y;
            if (x > max_x)
                max_x = x;
            if (y > max_y)
                max_y = y;
            if (y >= p->extent.height * 3 / 4)
                bottom_changed++;
        }
    }

    printf("present_smoke: readback %ux%u, reference 0x%08x, %u pixels changed",
           p->extent.width, p->extent.height, reference, changed);
    if (changed)
        printf(", bounding box (%u,%u)-(%u,%u)", min_x, min_y, max_x, max_y);
    printf("\n");

    if (!p->expect_overlay)
    {
        if (changed)
        {
            printf("FAIL the layer is off and still changed %u pixels\n", changed);
            status = 1;
        }
        else
            printf("PASS the layer is off and left the frame untouched\n");
    }
    else if (changed < 100)
    {
        printf("FAIL the overlay drew nothing into the presented image\n");
        status = 1;
    }
    else if (bottom_changed)
    {
        printf("FAIL %u changed pixels in the bottom quarter, the overlay is not confined\n",
               bottom_changed);
        status = 1;
    }
    else if (max_x >= p->extent.width * 3 / 4 || max_y >= p->extent.height * 3 / 4)
    {
        printf("FAIL the changed region reaches (%u,%u), further than the window should\n", max_x,
               max_y);
        status = 1;
    }
    else
    {
        printf("PASS the overlay drew %u pixels inside (%u,%u)-(%u,%u) and touched nothing "
               "outside it\n", changed, min_x, min_y, max_x, max_y);
    }

    /* A pixel count says the overlay drew; only an eye says it drew something
     * legible, so the frame can be dumped for one. */
    if (const char *dump = getenv("WINEPIPEWIRE_HUD_SMOKE_DUMP"))
    {
        FILE *out = fopen(dump, "wb");

        if (out)
        {
            fprintf(out, "P6\n%u %u\n255\n", p->extent.width, p->extent.height);
            for (uint32_t i = 0; i < p->extent.width * p->extent.height; i++)
                fwrite(pixels + (size_t)i * 4, 1, 3, out);
            fclose(out);
            printf("present_smoke: frame written to %s\n", dump);
        }
        else
            printf("present_smoke: cannot write %s\n", dump);
    }

    vkUnmapMemory(p->device, memory);
    vkDestroyBuffer(p->device, buffer, nullptr);
    vkFreeMemory(p->device, memory, nullptr);
    return status;
}

int main(int argc, char **argv)
{
    const uint32_t frames = argc > 1 ? (uint32_t)strtoul(argv[1], nullptr, 10) : 8;
    const unsigned publish_seconds = argc > 2 ? (unsigned)strtoul(argv[2], nullptr, 10) : 0;
    const char *instance_extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME,
                                          VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME };
    const char *device_extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    const float queue_priority = 1.0f;

    if (!has_instance_extension(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME))
    {
        printf("present_smoke: no %s, skipping\n", VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
        return 77;
    }

    /* Before vkCreateInstance, so the mapping exists by the first present and
     * the layer's once-a-second retry never has to fire. */
    struct hud_publisher pub;
    std::atomic<bool> publishing{ publish_seconds > 0 };
    std::thread publisher;

    if (publishing.load())
    {
        if (!pub.open())
            return 1;
        printf("present_smoke: publishing into %s for %us\n", pub.path, publish_seconds);
        publisher = std::thread(publish_loop, &pub, &publishing);
    }

    VkApplicationInfo app_info = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app_info.pApplicationName = "present_smoke";
    app_info.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instance_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instance_info.pApplicationInfo = &app_info;
    instance_info.enabledExtensionCount = 2;
    instance_info.ppEnabledExtensionNames = instance_extensions;

    VkInstance instance;
    CHECK(vkCreateInstance(&instance_info, nullptr, &instance));

    auto create_headless_surface = (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(
        instance, "vkCreateHeadlessSurfaceEXT");

    if (!create_headless_surface)
    {
        fprintf(stderr, "present_smoke: vkCreateHeadlessSurfaceEXT missing\n");
        return 1;
    }

    VkHeadlessSurfaceCreateInfoEXT surface_info = {
        VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT };
    VkSurfaceKHR surface;
    CHECK(create_headless_surface(instance, &surface_info, nullptr, &surface));

    uint32_t device_count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &device_count, nullptr));
    std::vector<VkPhysicalDevice> physical_devices(device_count);
    CHECK(vkEnumeratePhysicalDevices(instance, &device_count, physical_devices.data()));

    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    uint32_t queue_family = 0;

    for (VkPhysicalDevice candidate : physical_devices)
    {
        uint32_t family_count = 0;

        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());

        for (uint32_t i = 0; i < family_count; i++)
        {
            VkBool32 supported = VK_FALSE;

            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                continue;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &supported);
            if (!supported)
                continue;
            physical_device = candidate;
            queue_family = i;
            break;
        }
        if (physical_device)
            break;
    }

    if (!physical_device)
    {
        fprintf(stderr, "present_smoke: no device presents to a headless surface\n");
        return 1;
    }

    VkDeviceQueueCreateInfo queue_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;

    VkDeviceCreateInfo device_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = device_extensions;

    VkDevice device;
    CHECK(vkCreateDevice(physical_device, &device_info, nullptr, &device));

    VkQueue queue;
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &caps));
    /* TRANSFER_SRC is for the readback, TRANSFER_DST for the clear.  Neither is
     * COLOR_ATTACHMENT, so the layer still has to add that itself. */
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
    {
        fprintf(stderr, "present_smoke: surface rejects the transfer usages needed to read the "
                        "presented image back\n");
        return 1;
    }

    uint32_t format_count = 0;
    CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count,
                                               formats.data()));

    VkExtent2D extent = caps.currentExtent;

    /* When the surface leaves the extent to us, a small window rather than 512
     * square: the overlay draws a few hundred pixels wide once it draws bars, and
     * the confinement check below only means something when the frame is
     * comfortably larger than the panel.  Overridable so the same harness can
     * produce a frame at a resolution that scales the font. */
    if (extent.width == 0xffffffffu)
    {
        const char *env = getenv("WINEPIPEWIRE_HUD_SMOKE_EXTENT");
        unsigned w = 0, h = 0;

        extent = { 1024, 768 };
        if (env && sscanf(env, "%ux%u", &w, &h) == 2 && w && h)
            extent = { w, h };
        if (extent.width < caps.minImageExtent.width)
            extent.width = caps.minImageExtent.width;
        if (extent.height < caps.minImageExtent.height)
            extent.height = caps.minImageExtent.height;
        if (extent.width > caps.maxImageExtent.width)
            extent.width = caps.maxImageExtent.width;
        if (extent.height > caps.maxImageExtent.height)
            extent.height = caps.maxImageExtent.height;
    }

    VkSwapchainCreateInfoKHR swapchain_info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    swapchain_info.surface = surface;
    swapchain_info.minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount;
    swapchain_info.imageFormat = formats[0].format;
    swapchain_info.imageColorSpace = formats[0].colorSpace;
    swapchain_info.imageExtent = extent;
    swapchain_info.imageArrayLayers = 1;
    /* Deliberately not COLOR_ATTACHMENT: the layer has to add that itself. */
    swapchain_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_info.preTransform = caps.currentTransform;
    swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchain_info.clipped = VK_TRUE;

    VkSwapchainKHR swapchain;
    CHECK(vkCreateSwapchainKHR(device, &swapchain_info, nullptr, &swapchain));

    uint32_t image_count = 0;
    CHECK(vkGetSwapchainImagesKHR(device, swapchain, &image_count, nullptr));
    std::vector<VkImage> images(image_count);
    CHECK(vkGetSwapchainImagesKHR(device, swapchain, &image_count, images.data()));

    VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family;

    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &pool_info, nullptr, &pool));

    VkCommandBufferAllocateInfo cmd_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmd_info.commandPool = pool;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;

    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(device, &cmd_info, &cmd));

    VkSemaphoreCreateInfo semaphore_info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore acquired, rendered;
    CHECK(vkCreateSemaphore(device, &semaphore_info, nullptr, &acquired));
    CHECK(vkCreateSemaphore(device, &semaphore_info, nullptr, &rendered));

    /* With a publisher, the run is timed rather than counted: publish for the
     * requested seconds, then keep presenting for the tail so the consumer ages
     * the snapshot out and reports idle instead of frozen numbers.  A zero tail
     * publishes to the last frame instead, which is what a run that exists to be
     * looked at wants: the readback below then catches a live panel rather than
     * an idle one. */
    const char *tail_env = getenv("WINEPIPEWIRE_HUD_SMOKE_TAIL");
    const uint64_t tail_ns = (uint64_t)((tail_env ? atof(tail_env) : 2.0) * 1e9);
    const uint64_t deadline_ns =
        publish_seconds ? hud_mono_ns() + (uint64_t)publish_seconds * 1000000000ull + tail_ns : 0;
    uint32_t frame;

    for (frame = 0; deadline_ns ? hud_mono_ns() < deadline_ns : frame < frames; frame++)
    {
        VkClearColorValue clear = {};
        uint32_t image_index = 0;

        if (publishing.load() && publish_seconds && tail_ns &&
            hud_mono_ns() > deadline_ns - tail_ns)
        {
            printf("present_smoke: publisher stopping, the snapshot should age into idle\n");
            fflush(stdout);
            publishing.store(false);
            publisher.join();
        }
        /* One constant colour for every image, so any pixel that differs after
         * the present came from the overlay. */
        clear.float32[2] = 0.5f;

        CHECK(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE,
                                    &image_index));

        VkCommandBufferBeginInfo begin_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CHECK(vkResetCommandBuffer(cmd, 0));
        CHECK(vkBeginCommandBuffer(cmd, &begin_info));

        VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[image_index];
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        vkCmdClearColorImage(cmd, images[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear,
                             1, &barrier.subresourceRange);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
        CHECK(vkEndCommandBuffer(cmd));

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &rendered;
        CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));

        VkPresentInfoKHR present = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &rendered;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &image_index;
        CHECK(vkQueuePresentKHR(queue, &present));

        CHECK(vkQueueWaitIdle(queue));
        if (deadline_ns)
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    if (publishing.load())
    {
        publishing.store(false);
        publisher.join();
    }
    if (publish_seconds)
        pub.close_and_unlink();

    printf("present_smoke: %u frames, %u images, %ux%u, no colour attachment usage requested\n",
           frame, image_count, extent.width, extent.height);

    /* Acquire once more and read that image back.  Every image has been through
     * a present by now, so whichever one comes back carries an overlay. */
    uint32_t verify_index = 0;
    struct readback_params readback = {};
    int status;

    CHECK(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                &verify_index));
    readback.physical_device = physical_device;
    readback.device = device;
    readback.queue = queue;
    readback.cmd = cmd;
    readback.image = images[verify_index];
    readback.extent = extent;
    /* Same reading of the gate the layer itself makes, set and not "0", and the
     * same reading of the view level: an enabled layer asked to draw nothing must
     * leave the frame alone exactly as a disabled one does. */
    readback.expect_overlay = getenv(HUD_ENV_ENABLE) && *getenv(HUD_ENV_ENABLE) &&
                              strcmp(getenv(HUD_ENV_ENABLE), "0") &&
                              hud_view_level() != HUD_VIEW_OFF;
    status = verify_overlay_pixels(&readback);

    vkDestroySemaphore(device, rendered, nullptr);
    vkDestroySemaphore(device, acquired, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    return status;
}
