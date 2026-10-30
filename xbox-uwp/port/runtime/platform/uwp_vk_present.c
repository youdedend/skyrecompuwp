// Vulkan half of the presentation bridge. See uwp_vk_present.h for the why.
//
// Every constant here is taken from the console port that ships this (gow3's present_bridge.cpp):
// R8G8B8A8 UNORM, tiling OPTIMAL, usage COLOR_ATTACHMENT | TRANSFER_SRC | TRANSFER_DST, EXCLUSIVE
// sharing, a dedicated allocation, and VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT on both
// the image and the allocation.

// vulkan_win32.h is behind this define, and it has to precede vulkan.h.
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "uwp_present.h"
#include "uwp_vk_present.h"

struct UwpVkPresent {
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;

    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
    VkFormat format;
    uint32_t width, height;

    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;

    UwpPresenter *d3d;  // owned: created here, destroyed here
    PFN_vkGetMemoryWin32HandleKHR get_memory_handle;
    bool held_by_vulkan;  // false: D3D12 (or nobody) owns it
    bool initialised;     // false until the first acquire has defined the image's contents
};

static void present_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

// Free everything in reverse order. Safe on a partially built object, which is why every failure
// path can simply call it.
static void destroy_all(UwpVkPresent *p) {
    if (!p) return;
    if (p->dev) {
        if (p->fence) vkDestroyFence(p->dev, p->fence, NULL);
        if (p->pool) vkDestroyCommandPool(p->dev, p->pool, NULL);
        if (p->view) vkDestroyImageView(p->dev, p->view, NULL);
        if (p->memory && p->image) vkFreeMemory(p->dev, p->memory, NULL);
        if (p->image) vkDestroyImage(p->dev, p->image, NULL);
    }
    if (p->d3d) uwp_present_destroy(p->d3d);
    free(p);
}

// Runs one barrier on the presentation queue and waits for it. Small enough that the round trip is
// noise next to a frame, and simple enough that there is no state to get wrong.
static bool submit_barrier(UwpVkPresent *p, VkImageMemoryBarrier *barrier) {
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (vkResetCommandBuffer(p->cmd, 0) != VK_SUCCESS) return false;
    if (vkBeginCommandBuffer(p->cmd, &bi) != VK_SUCCESS) return false;
    vkCmdPipelineBarrier(p->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, NULL, 0, NULL, 1, barrier);
    if (vkEndCommandBuffer(p->cmd) != VK_SUCCESS) return false;

    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &p->cmd };
    if (vkResetFences(p->dev, 1, &p->fence) != VK_SUCCESS) return false;
    if (vkQueueSubmit(p->queue, 1, &si, p->fence) != VK_SUCCESS) return false;
    if (vkWaitForFences(p->dev, 1, &p->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false;
    return true;
}

// Hand the image's memory to D3D12. The handle belongs to the caller and is closed here.
static bool attach_to_d3d(UwpVkPresent *p) {
    VkMemoryGetWin32HandleInfoKHR ghi = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
        .memory = p->memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
    };
    HANDLE nt = NULL;
    if (p->get_memory_handle(p->dev, &ghi, &nt) != VK_SUCCESS || !nt) {
        present_log("[vk-present] vkGetMemoryWin32HandleKHR failed\n");
        return false;
    }
    bool attached = uwp_present_attach_vulkan_image(p->d3d, nt);
    CloseHandle(nt);
    if (!attached) present_log("[vk-present] the D3D12 side refused the shared image\n");
    return attached;
}

static bool make_image(UwpVkPresent *p, uint32_t width, uint32_t height) {
    VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
    };
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = p->format,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        // The renderer draws into it, the D3D12 copy reads it, and a reset may clear it.
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(p->dev, &ici, NULL, &p->image) != VK_SUCCESS) {
        present_log("[vk-present] vkCreateImage failed\n");
        return false;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(p->dev, p->image, &req);

    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(p->phys, &props);
    uint32_t type = props.memoryTypeCount;
    for (uint32_t i = 0; i < props.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
    if (type == props.memoryTypeCount) {
        present_log("[vk-present] no device-local memory type for the presentation image\n");
        return false;
    }

    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = p->image,
    };
    VkExportMemoryAllocateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_info,
        .allocationSize = req.size,
        .memoryTypeIndex = type,
    };
    if (vkAllocateMemory(p->dev, &mai, NULL, &p->memory) != VK_SUCCESS) {
        present_log("[vk-present] vkAllocateMemory (exportable) failed\n");
        return false;
    }
    if (vkBindImageMemory(p->dev, p->image, p->memory, 0) != VK_SUCCESS) {
        present_log("[vk-present] vkBindImageMemory failed\n");
        return false;
    }

    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = p->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = p->format,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    if (vkCreateImageView(p->dev, &vci, NULL, &p->view) != VK_SUCCESS) {
        present_log("[vk-present] vkCreateImageView failed\n");
        return false;
    }

    p->width = width;
    p->height = height;
    p->held_by_vulkan = false;
    p->initialised = false;
    present_log("[vk-present] presentation image %ux%u, %llu KiB\n", width, height,
                (unsigned long long)(req.size >> 10));
    return true;
}

UwpVkPresent *uwp_vk_present_create(VkPhysicalDevice phys, VkDevice dev, VkQueue queue,
                                    uint32_t queue_family, uint32_t width, uint32_t height,
                                    VkFormat format) {
    UwpVkPresent *p = (UwpVkPresent *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->phys = phys;
    p->dev = dev;
    p->queue = queue;
    p->qfam = queue_family;
    p->format = format;

    // Extension entry points are not exported by the loader; they are fetched per device.
    p->get_memory_handle =
        (PFN_vkGetMemoryWin32HandleKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryWin32HandleKHR");
    if (!p->get_memory_handle) {
        present_log("[vk-present] vkGetMemoryWin32HandleKHR is missing: no VK_KHR_external_memory_win32\n");
        destroy_all(p);
        return NULL;
    }

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .queueFamilyIndex = queue_family };
    if (vkCreateCommandPool(dev, &pci, NULL, &p->pool) != VK_SUCCESS) {
        present_log("[vk-present] vkCreateCommandPool failed\n");
        destroy_all(p);
        return NULL;
    }
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = p->pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    if (vkAllocateCommandBuffers(dev, &cai, &p->cmd) != VK_SUCCESS) {
        present_log("[vk-present] vkAllocateCommandBuffers failed\n");
        destroy_all(p);
        return NULL;
    }
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateFence(dev, &fci, NULL, &p->fence) != VK_SUCCESS) {
        present_log("[vk-present] vkCreateFence failed\n");
        destroy_all(p);
        return NULL;
    }

    if (!make_image(p, width, height)) {
        destroy_all(p);
        return NULL;
    }

    // The D3D12 side needs the CoreWindow already handed over by the activation host.
    p->d3d = uwp_present_create(width, height);
    if (!p->d3d) {
        present_log("[vk-present] could not create the D3D12 presenter\n");
        destroy_all(p);
        return NULL;
    }
    if (!attach_to_d3d(p)) {
        destroy_all(p);
        return NULL;
    }
    return p;
}

void uwp_vk_present_destroy(UwpVkPresent *p) { destroy_all(p); }

VkImage uwp_vk_present_image(UwpVkPresent *p) { return p ? p->image : VK_NULL_HANDLE; }
VkImageView uwp_vk_present_view(UwpVkPresent *p) { return p ? p->view : VK_NULL_HANDLE; }
VkFormat uwp_vk_present_format(UwpVkPresent *p) { return p ? p->format : VK_FORMAT_UNDEFINED; }

void uwp_vk_present_extent(UwpVkPresent *p, uint32_t *width, uint32_t *height) {
    if (width) *width = p ? p->width : 0;
    if (height) *height = p ? p->height : 0;
}

bool uwp_vk_present_begin(UwpVkPresent *p) {
    if (!p || p->held_by_vulkan) return false;
    // Take ownership from D3D12 and put the image in the layout the renderer draws in.
    // The renderer transitions the window image to COLOR_ATTACHMENT_OPTIMAL at frame start and leaves
    // it there (device.c), so that is the layout both transfers declare. D3D12 ignores layouts
    // entirely, which is why this only has to be consistent between the two Vulkan barriers.
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        // The very first acquire comes from UNDEFINED: the image has never been written, and
        // claiming it was already GENERAL is the kind of lie validation exists to catch.
        .oldLayout = p->initialised ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
        .dstQueueFamilyIndex = p->qfam,
        .image = p->image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    if (!submit_barrier(p, &b)) {
        present_log("[vk-present] acquire barrier failed\n");
        return false;
    }
    p->held_by_vulkan = true;
    p->initialised = true;
    return true;
}

bool uwp_vk_present_end(UwpVkPresent *p, bool vsync) {
    if (!p || !p->held_by_vulkan) return false;
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = p->qfam,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
        .image = p->image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    if (!submit_barrier(p, &b)) {
        present_log("[vk-present] release barrier failed\n");
        return false;
    }
    p->held_by_vulkan = false;
    return uwp_present_frame(p->d3d, vsync);
}

bool uwp_vk_present_resize(UwpVkPresent *p, uint32_t width, uint32_t height) {
    if (!p) return false;
    if (width == p->width && height == p->height) return true;
    // D3D12 must let go of the resource before Vulkan frees the memory behind it.
    vkDeviceWaitIdle(p->dev);
    if (!uwp_present_resize(p->d3d, width, height)) return false;
    if (p->view) vkDestroyImageView(p->dev, p->view, NULL);
    if (p->memory) vkFreeMemory(p->dev, p->memory, NULL);
    if (p->image) vkDestroyImage(p->dev, p->image, NULL);
    p->view = VK_NULL_HANDLE;
    p->image = VK_NULL_HANDLE;
    p->memory = VK_NULL_HANDLE;
    if (!make_image(p, width, height)) return false;
    return attach_to_d3d(p);
}
