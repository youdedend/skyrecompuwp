// Presentation bridge: Vulkan renders into an image that D3D12 copies to a CoreWindow swapchain.
//
// Why this exists: a UWP AppContainer has no HWND, so there is no Vulkan WSI surface to create -
// no vkCreateWin32SurfaceKHR, no VkSwapchainKHR, no vkAcquireNextImageKHR, no vkQueuePresentKHR.
// The working console port solves it by rendering into a Vulkan image backed by D3D12 shared
// memory, opening that same memory in D3D12, and copying it into a swapchain made for the
// CoreWindow. This is the same design, with the Vulkan half left to the renderer.
//
// Split of responsibility, deliberately:
//   * the renderer owns the Vulkan image (it has the device and picks the memory type), exports an
//     NT handle from it, and calls uwp_present_attach_vulkan_image();
//   * this file owns D3D12: device, queue, CoreWindow swapchain, the copy, the present.
//
// Everything here is legal in an AppContainer. Nothing here needs SDL, a window handle, or a
// Vulkan surface extension.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct UwpPresenter UwpPresenter;

// The CoreWindow to present into, as the ABI IUnknown* of Windows::UI::Core::CoreWindow. The
// activation host (uwp_entry.cpp) passes it on the UI thread before the renderer starts.
// Returns false when there is no window yet.
bool uwp_present_set_core_window(void *core_window_abi);

// Creates the D3D12 device, queue and CoreWindow swapchain. NULL on failure (reason in the log).
UwpPresenter *uwp_present_create(uint32_t width, uint32_t height);

// Takes ownership of nothing: `nt_handle` is a handle exported from the Vulkan image's memory
// (vkGetMemoryWin32HandleKHR with VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT). It is opened
// here with ID3D12Device::OpenSharedHandle and closed by the caller, as the Vulkan API requires.
bool uwp_present_attach_vulkan_image(UwpPresenter *p, void *nt_handle);

// Copy the attached image into the swapchain back buffer and present. Blocks until the copy has
// completed on the GPU, because the caller reuses the Vulkan image immediately afterwards.
bool uwp_present_frame(UwpPresenter *p, bool vsync);

bool uwp_present_resize(UwpPresenter *p, uint32_t width, uint32_t height);

void uwp_present_destroy(UwpPresenter *p);

#ifdef __cplusplus
}
#endif
