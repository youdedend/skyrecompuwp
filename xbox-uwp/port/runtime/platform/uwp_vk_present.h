// Vulkan half of the presentation bridge: the image the renderer draws into on Xbox.
//
// On the console there is no surface, so there is no swapchain either. Instead one image is created
// with D3D12-shareable memory, exported as an NT handle, and opened in D3D12 by uwp_present.cpp.
// The renderer draws into it every frame; D3D12 copies it to the CoreWindow swapchain.
//
// Ownership: the image belongs to exactly one of the two APIs at a time. Vulkan takes it with an
// acquire barrier, draws, and gives it back with a release barrier; only then may D3D12 read it.
// Getting this wrong does not fail loudly - it produces a frame that is occasionally a frame late,
// or torn - so both transfers are explicit and both are waited on.
//
// Reference for the shape: Cosmo/gow3_xbox xbox/graphics/present_bridge.cpp, which does the same
// thing in vulkan-hpp. This version submits each barrier on its own and waits, rather than carrying
// semaphores through the renderer's timeline: one extra fence wait per frame is a fair price for
// not threading semaphores into code that has its own model.
#pragma once

#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct UwpVkPresent UwpVkPresent;

// Creates the image, the memory and the D3D12 side. `format` must match the D3D12 swapchain's
// (DXGI_FORMAT_R8G8B8A8_UNORM, i.e. VK_FORMAT_R8G8B8A8_UNORM). Returns NULL on failure, with the
// reason in the log.
UwpVkPresent *uwp_vk_present_create(VkPhysicalDevice phys, VkDevice dev, VkQueue queue, uint32_t queue_family,
                                    uint32_t width, uint32_t height, VkFormat format);
void uwp_vk_present_destroy(UwpVkPresent *p);

// The image to render into, and its view. Valid until the next resize.
VkImage uwp_vk_present_image(UwpVkPresent *p);
VkImageView uwp_vk_present_view(UwpVkPresent *p);
VkFormat uwp_vk_present_format(UwpVkPresent *p);
void uwp_vk_present_extent(UwpVkPresent *p, uint32_t *width, uint32_t *height);

// Take ownership from D3D12 and leave the image ready to draw into (VK_IMAGE_LAYOUT_GENERAL).
bool uwp_vk_present_begin(UwpVkPresent *p);

// Give ownership back to D3D12, copy to the CoreWindow swapchain, present, and wait for that copy
// to finish so the next frame may draw into the image again.
bool uwp_vk_present_end(UwpVkPresent *p, bool vsync);

// Recreates the image at a new size. The D3D12 swapchain is resized first.
bool uwp_vk_present_resize(UwpVkPresent *p, uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif
