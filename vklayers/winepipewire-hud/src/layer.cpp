/*
 * Vulkan layer core for the winepipewire audio diagnostic overlay: loader
 * negotiation, dispatch chains, and the instance and device registries.
 *
 * SPDX-License-Identifier: MIT
 */
#include "layer.hpp"

#include <cstring>
#include <mutex>
#include <unordered_map>

#define HUD_EXPORT __attribute__((visibility("default")))

namespace {

std::mutex registry_lock;
std::unordered_map<void *, struct instance_data *> instances;
std::unordered_map<void *, struct device_data *> devices;
std::unordered_map<VkSwapchainKHR, struct swapchain_data *> swapchains;
std::unordered_map<VkQueue, struct hud_queue *> queue_map;

struct instance_data *instance_data_find(void *key)
{
    std::lock_guard<std::mutex> lock(registry_lock);
    auto it = instances.find(key);

    return it == instances.end() ? nullptr : it->second;
}

VkLayerInstanceCreateInfo *instance_chain_info(const VkInstanceCreateInfo *create_info,
                                               VkLayerFunction function)
{
    auto *item = (VkLayerInstanceCreateInfo *)create_info->pNext;

    while (item && (item->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
                    item->function != function))
        item = (VkLayerInstanceCreateInfo *)item->pNext;
    return item;
}

VkLayerDeviceCreateInfo *device_chain_info(const VkDeviceCreateInfo *create_info,
                                           VkLayerFunction function)
{
    auto *item = (VkLayerDeviceCreateInfo *)create_info->pNext;

    while (item && (item->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
                    item->function != function))
        item = (VkLayerDeviceCreateInfo *)item->pNext;
    return item;
}

VKAPI_ATTR VkResult VKAPI_CALL hud_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                  const VkAllocationCallbacks *pAllocator,
                                                  VkInstance *pInstance)
{
    VkLayerInstanceCreateInfo *chain = instance_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);

    if (!chain || !chain->u.pLayerInfo)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    auto create = (PFN_vkCreateInstance)next_gipa(nullptr, "vkCreateInstance");

    if (!create)
        return VK_ERROR_INITIALIZATION_FAILED;

    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    VkResult result = create(pCreateInfo, pAllocator, pInstance);

    if (result != VK_SUCCESS)
        return result;

    auto *data = new instance_data{};

    data->instance = *pInstance;
#define HUD_LOAD_INSTANCE(name) \
    data->vtable.name = (PFN_vk##name)next_gipa(*pInstance, "vk" #name);

    HUD_INSTANCE_FUNCS(HUD_LOAD_INSTANCE)
#undef HUD_LOAD_INSTANCE
    data->vtable.GetInstanceProcAddr = next_gipa;

    {
        std::lock_guard<std::mutex> lock(registry_lock);
        instances[dispatch_key(*pInstance)] = data;
    }

    HUD_LOG(HUD_LOG_LIFECYCLE, "instance %p created, %u application layers, %u extensions",
            (void *)*pInstance, pCreateInfo->enabledLayerCount, pCreateInfo->enabledExtensionCount);
    return result;
}

VKAPI_ATTR void VKAPI_CALL hud_DestroyInstance(VkInstance instance,
                                               const VkAllocationCallbacks *pAllocator)
{
    struct instance_data *data = instance_data_find(dispatch_key(instance));

    if (!data)
        return;

    data->vtable.DestroyInstance(instance, pAllocator);

    {
        std::lock_guard<std::mutex> lock(registry_lock);
        instances.erase(dispatch_key(instance));
    }
    HUD_LOG(HUD_LOG_LIFECYCLE, "instance %p destroyed", (void *)instance);
    delete data;
}

VKAPI_ATTR VkResult VKAPI_CALL hud_CreateDevice(VkPhysicalDevice physicalDevice,
                                                const VkDeviceCreateInfo *pCreateInfo,
                                                const VkAllocationCallbacks *pAllocator,
                                                VkDevice *pDevice)
{
    VkLayerDeviceCreateInfo *chain = device_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);
    VkLayerDeviceCreateInfo *loader_data = device_chain_info(pCreateInfo, VK_LOADER_DATA_CALLBACK);
    struct instance_data *instance = instance_data_find(dispatch_key(physicalDevice));

    if (!chain || !chain->u.pLayerInfo || !instance)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    auto create = (PFN_vkCreateDevice)next_gipa(nullptr, "vkCreateDevice");

    if (!create)
        return VK_ERROR_INITIALIZATION_FAILED;

    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    VkResult result = create(physicalDevice, pCreateInfo, pAllocator, pDevice);

    if (result != VK_SUCCESS)
        return result;

    auto *data = new device_data{};

    data->device = *pDevice;
    data->instance = instance;
    data->physical_device = physicalDevice;
    data->set_device_loader_data = loader_data ? loader_data->u.pfnSetDeviceLoaderData : nullptr;
#define HUD_LOAD_DEVICE(name) \
    data->vtable.name = (PFN_vk##name)next_gdpa(*pDevice, "vk" #name);

    HUD_DEVICE_FUNCS(HUD_LOAD_DEVICE)
#undef HUD_LOAD_DEVICE
    data->vtable.GetDeviceProcAddr = next_gdpa;

    instance->vtable.GetPhysicalDeviceProperties(physicalDevice, &data->properties);

    {
        std::lock_guard<std::mutex> lock(registry_lock);
        devices[dispatch_key(*pDevice)] = data;
    }

    /* Needs the device registered first: the queues it maps are looked up
     * through the same registry. */
    if (hud_enabled() && data->set_device_loader_data)
        hud_device_map_queues(data, pCreateInfo);

    HUD_LOG(HUD_LOG_LIFECYCLE,
            "device %p created on \"%s\", api %u.%u.%u, %zu queues, graphics family %d",
            (void *)*pDevice, data->properties.deviceName,
            VK_API_VERSION_MAJOR(data->properties.apiVersion),
            VK_API_VERSION_MINOR(data->properties.apiVersion),
            VK_API_VERSION_PATCH(data->properties.apiVersion), data->render.queues.size(),
            data->render.graphic_queue ? (int)data->render.graphic_queue->family_index : -1);
    return result;
}

VKAPI_ATTR void VKAPI_CALL hud_DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
    struct device_data *data = device_data_find(dispatch_key(device));

    if (!data)
        return;

    hud_device_unmap_queues(data);
    data->vtable.DestroyDevice(device, pAllocator);

    {
        std::lock_guard<std::mutex> lock(registry_lock);
        devices.erase(dispatch_key(device));
    }
    HUD_LOG(HUD_LOG_LIFECYCLE, "device %p destroyed", (void *)device);
    delete data;
}

struct hook_entry
{
    const char *name;
    PFN_vkVoidFunction ptr;
};

#define HUD_HOOK(name) { "vk" #name, reinterpret_cast<PFN_vkVoidFunction>(hud_##name) }

/* Always intercepted: the chain has to be advanced and the dispatch tables
 * built whether or not the overlay does anything. */
const struct hook_entry chain_hooks[] = {
    HUD_HOOK(CreateInstance),
    HUD_HOOK(DestroyInstance),
    HUD_HOOK(CreateDevice),
    HUD_HOOK(DestroyDevice),
};

/* Withheld while the overlay is off, which leaves the application's calls
 * untouched rather than passed through us. */
const struct hook_entry overlay_hooks[] = {
    HUD_HOOK(CreateSwapchainKHR),
    HUD_HOOK(DestroySwapchainKHR),
    HUD_HOOK(QueuePresentKHR),
};

#undef HUD_HOOK

PFN_vkVoidFunction find_hook(const char *name)
{
    for (const struct hook_entry &hook : chain_hooks)
        if (!strcmp(name, hook.name))
            return hook.ptr;

    if (!hud_enabled())
        return nullptr;

    for (const struct hook_entry &hook : overlay_hooks)
        if (!strcmp(name, hook.name))
            return hook.ptr;
    return nullptr;
}

} /* namespace */

struct device_data *device_data_find(void *key)
{
    std::lock_guard<std::mutex> lock(registry_lock);
    auto it = devices.find(key);

    return it == devices.end() ? nullptr : it->second;
}

struct hud_queue *hud_queue_register(struct device_data *device_data, VkQueue queue,
                                     VkQueueFlags flags, uint32_t family_index)
{
    auto *data = new hud_queue{};

    data->device = device_data;
    data->queue = queue;
    data->flags = flags;
    data->family_index = family_index;

    std::lock_guard<std::mutex> lock(registry_lock);
    queue_map[queue] = data;
    return data;
}

struct hud_queue *hud_queue_find(VkQueue queue)
{
    std::lock_guard<std::mutex> lock(registry_lock);
    auto it = queue_map.find(queue);

    return it == queue_map.end() ? nullptr : it->second;
}

void hud_queue_unregister(VkQueue queue)
{
    std::lock_guard<std::mutex> lock(registry_lock);

    queue_map.erase(queue);
}

struct swapchain_data *swapchain_data_create(VkSwapchainKHR swapchain, struct device_data *device,
                                             const VkSwapchainCreateInfoKHR *create_info,
                                             bool app_color_attachment)
{
    auto *data = new swapchain_data{};

    data->swapchain = swapchain;
    data->device = device;
    data->extent = create_info->imageExtent;
    data->format = create_info->imageFormat;
    data->color_space = create_info->imageColorSpace;
    data->app_color_attachment = app_color_attachment;
    data->report_ns = hud_mono_ns();

    if (device->vtable.GetSwapchainImagesKHR)
        device->vtable.GetSwapchainImagesKHR(device->device, swapchain, &data->image_count, nullptr);

    std::lock_guard<std::mutex> lock(registry_lock);
    swapchains[swapchain] = data;
    return data;
}

struct swapchain_data *swapchain_data_find(VkSwapchainKHR swapchain)
{
    std::lock_guard<std::mutex> lock(registry_lock);
    auto it = swapchains.find(swapchain);

    return it == swapchains.end() ? nullptr : it->second;
}

void swapchain_data_destroy(VkSwapchainKHR swapchain)
{
    struct swapchain_data *data;

    {
        std::lock_guard<std::mutex> lock(registry_lock);
        auto it = swapchains.find(swapchain);

        if (it == swapchains.end())
            return;
        data = it->second;
        swapchains.erase(it);
    }
    delete data;
}

extern "C" HUD_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
    PFN_vkVoidFunction hook = find_hook(pName);

    if (hook)
        return hook;
    if (!strcmp(pName, "vkGetDeviceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    if (!device)
        return nullptr;

    struct device_data *data = device_data_find(dispatch_key(device));

    if (!data || !data->vtable.GetDeviceProcAddr)
        return nullptr;
    return data->vtable.GetDeviceProcAddr(device, pName);
}

extern "C" HUD_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    PFN_vkVoidFunction hook = find_hook(pName);

    if (hook)
        return hook;
    if (!strcmp(pName, "vkGetInstanceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
    if (!strcmp(pName, "vkGetDeviceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    if (!instance)
        return nullptr;

    struct instance_data *data = instance_data_find(dispatch_key(instance));

    if (!data || !data->vtable.GetInstanceProcAddr)
        return nullptr;
    return data->vtable.GetInstanceProcAddr(instance, pName);
}

extern "C" HUD_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct)
{
    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (pVersionStruct->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;

    pVersionStruct->loaderLayerInterfaceVersion = 2;
    pVersionStruct->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;

    HUD_LOG(HUD_LOG_LIFECYCLE, "%s negotiated loader interface 2, overlay %s", HUD_LAYER_NAME,
            hud_enabled() ? "enabled" : "off (" HUD_ENV_ENABLE " unset)");
    return VK_SUCCESS;
}
