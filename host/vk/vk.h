// The Vulkan back end: what the replay of NVN recordings shares (vk_device.cpp: the device and the screen;
// vk_memory.cpp: guest memory as buffers; vk_texture.cpp: textures and samplers; vk_shader.cpp: programs;
// vk_replay.cpp: the rx_nvn_* entry points and the replay).
//
// Design (oranges): one device queue, and each submit replayed into one command buffer that is run to completion before
// the submit returns, so the GPU's work is done in the game's order and every fence is reached at once, as with the
// null back end. Guest memory pools are imported into Vulkan (VK_EXT_external_memory_host), so buffers (vertex, index,
// uniform, storage) are the game's own memory and GPU writes land in it. Textures are Vulkan images in device memory,
// filled from guest memory (deswizzled) at their first use. Shaders are shader objects (VK_EXT_shader_object) with
// every state dynamic, so no pipelines are made; descriptors are pushed (VK_KHR_push_descriptor). Images stay in
// VK_IMAGE_LAYOUT_GENERAL, and work is ordered with full barriers between render passes, copies and dispatches.
#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>

#include "../nvn/rx_nvn.h"
#include "vk_log.h"

extern "C" int64_t hos_now_ns(void);

namespace ovk {

struct Ext {
#define OVK_EXT_FUNCS(X)                                                                                               \
    X(vkCreateShadersEXT) X(vkDestroyShaderEXT) X(vkCmdBindShadersEXT) X(vkCmdSetVertexInputEXT)                      \
    X(vkCmdPushDescriptorSetKHR) X(vkCmdSetPolygonModeEXT) X(vkCmdSetRasterizationSamplesEXT) X(vkCmdSetSampleMaskEXT)  \
    X(vkCmdSetAlphaToCoverageEnableEXT) X(vkCmdSetAlphaToOneEnableEXT) X(vkCmdSetLogicOpEnableEXT)                    \
    X(vkCmdSetColorBlendEnableEXT) X(vkCmdSetColorBlendEquationEXT) X(vkCmdSetColorWriteMaskEXT)                     \
    X(vkCmdSetDepthClampEnableEXT) X(vkCmdSetDepthClipNegativeOneToOneEXT) X(vkCmdSetLogicOpEXT)                      \
    X(vkCmdSetPatchControlPointsEXT) X(vkCmdSetTessellationDomainOriginEXT) X(vkCmdSetDepthClipEnableEXT)             \
    X(vkGetMemoryHostPointerPropertiesEXT)
#define OVK_DECLARE(f) PFN_##f f = nullptr;
    OVK_EXT_FUNCS(OVK_DECLARE)
#undef OVK_DECLARE
};

struct Gpu {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties mem{};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    void* window = nullptr;   // (SDL_Window: null when headless)
    uint64_t host_align = 4096;   // minImportedHostPointerAlignment
    bool tessellation = false, geometry = false, custom_border = false;
    bool view_2d_of_3d = false;   // (VK_EXT_image_2d_view_of_3d: a 3D texture sampled as 2D, its first slice)
    Ext x;
};
extern Gpu g;
/// the back end's lock: the replay, the caches and the queue (the game's threads submit and present concurrently)
extern std::recursive_mutex big_lock;

uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want);
/// a buffer of host-visible memory (staging, driver banks), mapped
struct HostBuffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint8_t* map = nullptr;
    uint64_t size = 0;
};
HostBuffer host_buffer(uint64_t size, VkBufferUsageFlags usage);
void free_host_buffer(HostBuffer& b);

/// a one-time command buffer: begin, then end_and_wait runs it to completion
VkCommandBuffer begin_commands();
void end_and_wait(VkCommandBuffer cb);
/// a full memory barrier (everything before against everything after)
void full_barrier(VkCommandBuffer cb);

// ---- the screen (vk_device.cpp)
/// the window texture's image to the screen (and HOS_SCREENSHOT), from a command buffer about to be ended
void present_image(VkImage image, VkFormat format, uint32_t w, uint32_t h, const int crop[4]);

// ---- guest memory (vk_memory.cpp)
struct BufRef {
    VkBuffer buf = VK_NULL_HANDLE;
    uint64_t offset = 0, size = 0;   // (size: from offset to the end of what is imported, or what was asked)
};
/// the imported buffer holding [gpu, gpu + size): a null buf when it is not imported
BufRef buffer_at(uint64_t gpu, uint64_t size);
void pool_created(uint64_t gpu, uint64_t size);

// ---- textures and samplers (vk_texture.cpp)
struct Image;
struct FormatInfo {
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool depth = false, stencil = false, integer = false, sint = false, compressed = false, astc = false;
};
FormatInfo format_info(int nvn_format);
/// the image of a texture (made, and filled from guest memory, at its first use); null when the format has no image
Image* image_of(VkCommandBuffer cb, const nvn_tex* t);
/// a view of it (the view's levels, layers, format and swizzle; shader_type: the Shader::TextureType the shader
/// declares; for_attachment: one level and layer, no swizzle)
VkImageView view_of(Image* im, const nvn_view* v, bool has_view, int shader_type, bool for_attachment);
VkImage image_handle(Image* im);
VkFormat image_format(Image* im);
void image_extent(Image* im, uint32_t level, uint32_t* w, uint32_t* h);
/// the image was written by the GPU (a render target, a copy, a clear): never filled from guest memory again
void image_written(Image* im);
VkSampler sampler_of(int id);
/// the texture and view a texture pool id is registered with
bool texture_of(int id, nvn_tex* t, nvn_view* v, bool* has_view);
void textures_frame_end();

}  // namespace ovk
