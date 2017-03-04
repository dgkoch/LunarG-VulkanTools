/*
 *
 * Copyright (C) 2015-2017 Valve Corporation
 * Copyright (C) 2015-2017 LunarG, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: Arda Coskunses <arda@lunarg.com>
 */
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <unordered_map>
#include "loader.h"
#include "vk_dispatch_table_helper.h"
#include "vulkan/vk_lunarg_device_profile_api-layer.h"
#include "vulkan/vk_device_profile_api_layer.h"
#include "vulkan/vk_layer.h"
#include "vk_layer_table.h"
#include "mutex"

namespace device_profile_api {

static std::unordered_map<dispatch_key, VkInstance> device_profile_api_instance_map;
static std::mutex global_lock;

// device_profile_api Layer data to store profiled GPU information
struct device_profile_api_data {
    VkPhysicalDeviceProperties *props;
    VkQueueFamilyProperties *queue_props;
    VkDeviceQueueCreateInfo *queue_reqs;
    VkPhysicalDeviceMemoryProperties memory_props;
    VkPhysicalDeviceFeatures features;
    uint32_t device_extension_count;
    VkExtensionProperties *device_extensions;
};
static std::unordered_map<VkPhysicalDevice, struct device_profile_api_data> device_profile_api_dev_data_map;

// device_profile_api Layer EXT APIs
typedef VkResult(VKAPI_PTR *PFN_vkGetOriginalPhysicalDeviceLimitsEXT)(VkPhysicalDevice physicalDevice,
                                                                      const VkPhysicalDeviceLimits *limits);
static PFN_vkGetOriginalPhysicalDeviceLimitsEXT pfn_get_original_device_limits_extension;

typedef VkResult(VKAPI_PTR *PFN_vkSetPhysicalDeviceLimitsEXT)(VkPhysicalDevice physicalDevice,
                                                              const VkPhysicalDeviceLimits *newLimits);
static PFN_vkSetPhysicalDeviceLimitsEXT pfn_set_physical_device_limits_extension;

VKAPI_ATTR void VKAPI_CALL GetOriginalPhysicalDeviceLimitsEXT(VkPhysicalDevice physicalDevice, VkPhysicalDeviceLimits *orgLimits) {
    // unwrapping the physicalDevice in order to get the same physicalDevice address which loader wraps
    // We have to do this as this API is a layer extension and loader does not wrap like regular API in vulkan.h
    VkPhysicalDevice unwrapped_phys_dev = loader_unwrap_physical_device(physicalDevice);
    {
        std::lock_guard<std::mutex> lock(global_lock);

        VkPhysicalDeviceProperties implicit_properties;
        instance_dispatch_table(unwrapped_phys_dev)->GetPhysicalDeviceProperties(unwrapped_phys_dev, &implicit_properties);

        if (orgLimits) memcpy(orgLimits, &implicit_properties.limits, sizeof(VkPhysicalDeviceLimits));
    }
}

VKAPI_ATTR VkResult VKAPI_CALL SetPhysicalDeviceLimitsEXT(VkPhysicalDevice physicalDevice,
                                                          const VkPhysicalDeviceLimits *newLimits) {
    // unwrapping the physicalDevice in order to get the same physicalDevice address which loader wraps
    // We have to do this as this API is a layer extension and loader does not wrap like regular API in vulkan.h
    VkPhysicalDevice unwrapped_phys_dev = loader_unwrap_physical_device(physicalDevice);
    {
        std::lock_guard<std::mutex> lock(global_lock);

        // search if we got the device limits for this device and stored in device_profile_api layer
        auto device_profile_api_data_it = device_profile_api_dev_data_map.find(unwrapped_phys_dev);
        // if we do not have it call getDeviceProperties implicitly and store device properties in the device_profile_api_layer
        if (device_profile_api_data_it != device_profile_api_dev_data_map.end()) {
            if (device_profile_api_dev_data_map[unwrapped_phys_dev].props && newLimits)
                memcpy(&(device_profile_api_dev_data_map[unwrapped_phys_dev].props->limits), newLimits, sizeof(VkPhysicalDeviceLimits));
        }
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
                                              VkInstance *pInstance) {
    VkLayerInstanceCreateInfo *chain_info = get_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);
    std::lock_guard<std::mutex> lock(global_lock);

    assert(chain_info->u.pLayerInfo);
    PFN_vkGetInstanceProcAddr fp_get_instance_proc_addr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkCreateInstance fp_create_instance = (PFN_vkCreateInstance)fp_get_instance_proc_addr(NULL, "vkCreateInstance");
    if (fp_create_instance == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Advance the link info for the next element on the chain
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    VkResult result = fp_create_instance(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) return result;

    device_profile_api_instance_map[get_dispatch_key(*pInstance)] = *pInstance;
    initInstanceTable(*pInstance, fp_get_instance_proc_addr);

    uint32_t physicalDeviceCount = 0;
    VkPhysicalDevice *physicalDevices;
    result = instance_dispatch_table(*pInstance)->EnumeratePhysicalDevices(*pInstance, &physicalDeviceCount, NULL);
    if (result != VK_SUCCESS) return result;

    physicalDevices = (VkPhysicalDevice *)malloc(sizeof(physicalDevices[0]) * physicalDeviceCount);
    result = instance_dispatch_table(*pInstance)->EnumeratePhysicalDevices(*pInstance, &physicalDeviceCount, physicalDevices);
    if (result != VK_SUCCESS) return result;

    // First of all get original physical device limits
    for (uint8_t i = 0; i < physicalDeviceCount; i++) {
        // Search if we got the device limits for this device and stored in device_profile_api layer
        auto device_profile_api_data_it = device_profile_api_dev_data_map.find(physicalDevices[i]);
        // If we do not have it store device properties in the device_profile_api_layer
        if (device_profile_api_data_it == device_profile_api_dev_data_map.end()) {
            device_profile_api_dev_data_map[physicalDevices[i]].props = (VkPhysicalDeviceProperties *)malloc(sizeof(VkPhysicalDeviceProperties));
            if (device_profile_api_dev_data_map[physicalDevices[i]].props) {
                instance_dispatch_table(*pInstance)
                    ->GetPhysicalDeviceProperties(physicalDevices[i], device_profile_api_dev_data_map[physicalDevices[i]].props);
            } else {
                if (i == 0) {
                    return VK_ERROR_OUT_OF_HOST_MEMORY;
                } else {  // Free others
                    for (uint8_t j = 0; j < i; j++) {
                        if (device_profile_api_dev_data_map[physicalDevices[j]].props) {
                            free(device_profile_api_dev_data_map[physicalDevices[j]].props);
                            device_profile_api_dev_data_map[physicalDevices[j]].props = nullptr;
                        }
                    }
                    return VK_ERROR_OUT_OF_HOST_MEMORY;
                }
            }
        }
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDevices(VkInstance instance, uint32_t *pPhysicalDeviceCount,
                                                        VkPhysicalDevice *pPhysicalDevices) {
    VkResult result = instance_dispatch_table(instance)->EnumeratePhysicalDevices(instance, pPhysicalDeviceCount, pPhysicalDevices);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
                                            const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
    VkLayerDeviceCreateInfo *chain_info = get_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);

    assert(chain_info->u.pLayerInfo);
    PFN_vkGetInstanceProcAddr fp_get_instance_proc_addr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr fp_get_device_proc_addr = chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    VkInstance instance = device_profile_api_instance_map[get_dispatch_key(physicalDevice)];
    PFN_vkCreateDevice fp_create_device = (PFN_vkCreateDevice)fp_get_instance_proc_addr(instance, "vkCreateDevice");
    if (fp_create_device == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Advance the link info for the next element on the chain
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    VkResult result = fp_create_device(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) {
        return result;
    }

    initDeviceTable(*pDevice, fp_get_device_proc_addr);

    pfn_get_original_device_limits_extension =
        (PFN_vkGetOriginalPhysicalDeviceLimitsEXT)fp_get_device_proc_addr(*pDevice, "vkGetOriginalPhysicalDeviceLimitsEXT");
    pfn_set_physical_device_limits_extension =
        (PFN_vkSetPhysicalDeviceLimitsEXT)fp_get_device_proc_addr(*pDevice, "vkSetPhysicalDeviceLimitsEXT");
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
    dispatch_key key = get_dispatch_key(device);
    device_dispatch_table(device)->DestroyDevice(device, pAllocator);
    destroy_device_dispatch_table(key);
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator) {
    dispatch_key key = get_dispatch_key(instance);
    instance_dispatch_table(instance)->DestroyInstance(instance, pAllocator);
    destroy_instance_dispatch_table(key);
    device_profile_api_instance_map.erase(key);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures *pFeatures) {
    instance_dispatch_table(physicalDevice)->GetPhysicalDeviceFeatures(physicalDevice, pFeatures);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format,
                                                             VkFormatProperties *pFormatProperties) {
    instance_dispatch_table(physicalDevice)->GetPhysicalDeviceFormatProperties(physicalDevice, format, pFormatProperties);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format,
                                                                  VkImageType type, VkImageTiling tiling,
                                                                  VkImageUsageFlags usage, VkImageCreateFlags flags,
                                                                  VkImageFormatProperties *pImageFormatProperties) {
    instance_dispatch_table(physicalDevice)
        ->GetPhysicalDeviceImageFormatProperties(physicalDevice, format, type, tiling, usage, flags, pImageFormatProperties);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                                       VkPhysicalDeviceProperties *pProperties) {
    {
        std::lock_guard<std::mutex> lock(global_lock);

        // Search if we got the device limits for this device and stored in device_profile_api layer
        auto device_profile_api_data_it = device_profile_api_dev_data_map.find(physicalDevice);
        if (device_profile_api_data_it != device_profile_api_dev_data_map.end()) {
            // device_profile_api layer device limits exists for this device so overwrite with desired limits
            if (device_profile_api_dev_data_map[physicalDevice].props)
                memcpy(pProperties, device_profile_api_dev_data_map[physicalDevice].props, sizeof(VkPhysicalDeviceProperties));
        } else {
            instance_dispatch_table(physicalDevice)->GetPhysicalDeviceProperties(physicalDevice, pProperties);
        }
    }
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice,
                                                                  uint32_t *pQueueFamilyPropertyCount,
                                                                  VkQueueFamilyProperties *pQueueFamilyProperties) {
    instance_dispatch_table(physicalDevice)
        ->GetPhysicalDeviceQueueFamilyProperties(physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
                                                             VkPhysicalDeviceMemoryProperties *pMemoryProperties) {
    instance_dispatch_table(physicalDevice)->GetPhysicalDeviceMemoryProperties(physicalDevice, pMemoryProperties);
}

static const VkLayerProperties device_profile_api_LayerProps = {
    "VK_LAYER_LUNARG_device_profile_api", VK_MAKE_VERSION(1, 0, VK_HEADER_VERSION),  // specVersion
    1,                                                                  // implementationVersion
    "LunarG device profile api Layer",
};

static const VkExtensionProperties device_profile_api_physicaldevice_extensions[] = {{
    "vkLayerdevice_profile_apiEXT", 1,
}};

template <typename T>
VkResult EnumerateProperties(uint32_t src_count, const T *src_props, uint32_t *dst_count, T *dst_props) {
    if (!dst_props || !src_props) {
        *dst_count = src_count;
        return VK_SUCCESS;
    }

    uint32_t copy_count = (*dst_count < src_count) ? *dst_count : src_count;
    memcpy(dst_props, src_props, sizeof(T) * copy_count);
    *dst_count = copy_count;

    return (copy_count == src_count) ? VK_SUCCESS : VK_INCOMPLETE;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceLayerProperties(uint32_t *pCount, VkLayerProperties *pProperties) {
    return EnumerateProperties(1, &device_profile_api_LayerProps, pCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice, uint32_t *pCount,
                                                              VkLayerProperties *pProperties) {
    return EnumerateProperties(1, &device_profile_api_LayerProps, pCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pCount,
                                                                    VkExtensionProperties *pProperties) {
    if (pLayerName && !strcmp(pLayerName, device_profile_api_LayerProps.layerName))
        return EnumerateProperties<VkExtensionProperties>(0, NULL, pCount, pProperties);

    return VK_ERROR_LAYER_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char *pLayerName,
                                                                  uint32_t *pCount, VkExtensionProperties *pProperties) {
    if (pLayerName && !strcmp(pLayerName, device_profile_api_LayerProps.layerName)) {
        uint32_t count = sizeof(device_profile_api_physicaldevice_extensions) / sizeof(device_profile_api_physicaldevice_extensions[0]);
        return EnumerateProperties(count, device_profile_api_physicaldevice_extensions, pCount, pProperties);
    }

    return instance_dispatch_table(physicalDevice)->EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pCount, pProperties);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char *name) {
    if (!name || name[0] != 'v' || name[1] != 'k') return NULL;

    name += 2;
    if (!strcmp(name, "GetDeviceProcAddr")) return (PFN_vkVoidFunction)GetDeviceProcAddr;
    if (!strcmp(name, "DestroyDevice")) return (PFN_vkVoidFunction)DestroyDevice;
    if (device == NULL) return NULL;

    if (device_dispatch_table(device)->GetDeviceProcAddr == NULL) return NULL;
    return device_dispatch_table(device)->GetDeviceProcAddr(device, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char *name) {
    if (!name || name[0] != 'v' || name[1] != 'k') return NULL;

    name += 2;
    if (!strcmp(name, "CreateInstance")) return (PFN_vkVoidFunction)CreateInstance;
    if (!strcmp(name, "DestroyInstance")) return (PFN_vkVoidFunction)DestroyInstance;
    if (!strcmp(name, "EnumeratePhysicalDevices")) return (PFN_vkVoidFunction)EnumeratePhysicalDevices;
    if (!strcmp(name, "GetPhysicalDeviceFeatures")) return (PFN_vkVoidFunction)GetPhysicalDeviceFeatures;
    if (!strcmp(name, "GetPhysicalDeviceFormatProperties")) return (PFN_vkVoidFunction)GetPhysicalDeviceFormatProperties;
    if (!strcmp(name, "GetPhysicalDeviceImageFormatProperties")) return (PFN_vkVoidFunction)GetPhysicalDeviceImageFormatProperties;
    if (!strcmp(name, "GetPhysicalDeviceProperties")) return (PFN_vkVoidFunction)GetPhysicalDeviceProperties;
    if (!strcmp(name, "GetPhysicalDeviceQueueFamilyProperties")) return (PFN_vkVoidFunction)GetPhysicalDeviceQueueFamilyProperties;
    if (!strcmp(name, "GetPhysicalDeviceMemoryProperties")) return (PFN_vkVoidFunction)GetPhysicalDeviceMemoryProperties;
    if (!strcmp(name, "GetInstanceProcAddr")) return (PFN_vkVoidFunction)GetInstanceProcAddr;
    if (!strcmp(name, "CreateDevice")) return (PFN_vkVoidFunction)CreateDevice;
    if (!strcmp(name, "EnumerateInstanceExtensionProperties")) return (PFN_vkVoidFunction)EnumerateInstanceExtensionProperties;
    if (!strcmp(name, "EnumerateInstanceLayerProperties")) return (PFN_vkVoidFunction)EnumerateInstanceLayerProperties;
    if (!strcmp(name, "EnumerateDeviceLayerProperties")) return (PFN_vkVoidFunction)EnumerateDeviceLayerProperties;
    if (!strcmp(name, "EnumerateDeviceExtensionProperties")) return (PFN_vkVoidFunction)EnumerateDeviceExtensionProperties;
    if (!strcmp(name, "SetPhysicalDeviceLimitsEXT")) return (PFN_vkVoidFunction)SetPhysicalDeviceLimitsEXT;
    if (!strcmp(name, "GetOriginalPhysicalDeviceLimitsEXT")) return (PFN_vkVoidFunction)GetOriginalPhysicalDeviceLimitsEXT;

    assert(instance);

    PFN_vkVoidFunction proc = GetDeviceProcAddr(VK_NULL_HANDLE, name);
    if (proc) return proc;

    if (instance_dispatch_table(instance)->GetInstanceProcAddr == NULL) return NULL;
    return instance_dispatch_table(instance)->GetInstanceProcAddr(instance, name);
}

} // namespace device_profile_api

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *pCount,
                                                                                  VkLayerProperties *pProperties) {
    return device_profile_api::EnumerateInstanceLayerProperties(pCount, pProperties);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice, uint32_t *pCount,
                                                                                VkLayerProperties *pProperties) {
    assert(physicalDevice == VK_NULL_HANDLE);
    return device_profile_api::EnumerateDeviceLayerProperties(VK_NULL_HANDLE, pCount, pProperties);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pCount,
                                                                                      VkExtensionProperties *pProperties) {
    return device_profile_api::EnumerateInstanceExtensionProperties(pLayerName, pCount, pProperties);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                                                                    const char *pLayerName, uint32_t *pCount,
                                                                                    VkExtensionProperties *pProperties) {
    assert(physicalDevice == VK_NULL_HANDLE);
    return device_profile_api::EnumerateDeviceExtensionProperties(VK_NULL_HANDLE, pLayerName, pCount, pProperties);
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice dev, const char *funcName) {
    return device_profile_api::GetDeviceProcAddr(dev, funcName);
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *funcName) {
    return device_profile_api::GetInstanceProcAddr(instance, funcName);
}
