// The Vulkan device and the screen: instance, device, queue, the swapchain on the SDL window (or none, headless), and
// the present of a window texture (letterboxed into the window), plus HOS_SCREENSHOT=<file.bmp>[,seconds].
//
//   HOS_VK=0            the null back end (nothing drawn), as before
//   HOS_VK_DEVICE=<n>   that physical device (vulkaninfo's order); default: the first discrete GPU
//   HOS_VK_VALIDATE=1   the Khronos validation layer
//   HOS_VSYNC=1         FIFO presents (default: mailbox, else immediate: the game paces itself)
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "vk.h"

namespace ovk {

Gpu g;
std::recursive_mutex big_lock;

namespace {

bool env_on(const char* name) {
    const char* s = getenv(name);
    return s && *s && strcmp(s, "0") != 0;
}

#define CHECK(call)                                                                                                    \
    do {                                                                                                               \
        const VkResult r_ = (call);                                                                                    \
        if (r_ != VK_SUCCESS) {                                                                                        \
            hos_log("vk: %s failed: %d", #call, (int)r_);                                                              \
            return false;                                                                                              \
        }                                                                                                              \
    } while (0)

bool has_ext(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const auto& e : list)
        if (!strcmp(e.extensionName, name)) return true;
    return false;
}

// ---- the swapchain ----
struct Swapchain {
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    VkSemaphore acquired = VK_NULL_HANDLE;
    std::vector<VkSemaphore> done;   // (one per image: a present waits on it)
    bool srgb = false, stale = true;
} sw;

void destroy_swapchain() {
    if (!sw.sc) return;
    vkDeviceWaitIdle(g.dev);
    for (auto s : sw.done) vkDestroySemaphore(g.dev, s, nullptr);
    sw.done.clear();
    vkDestroySwapchainKHR(g.dev, sw.sc, nullptr);
    sw.sc = VK_NULL_HANDLE;
}

bool make_swapchain(bool srgb) {
    destroy_swapchain();
    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surface, &caps));
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &n, formats.data());
    VkSurfaceFormatKHR pick = formats.empty() ? VkSurfaceFormatKHR{} : formats[0];
    for (const auto& f : formats) {
        const bool want = srgb ? (f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB)
                               : (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM);
        if (want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            pick = f;
            break;
        }
    }
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.phys, g.surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.phys, g.surface, &n, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!env_on("HOS_VSYNC"))
        for (const VkPresentModeKHR want : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
            bool found = false;
            for (auto m : modes) found |= m == want;
            if (found) {
                mode = want;
                break;
            }
        }
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        int w = 1280, h = 720;
        SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(g.window), &w, &h);
        extent = {(uint32_t)w, (uint32_t)h};
    }
    if (!extent.width || !extent.height) return false;   // (minimized)
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g.surface;
    ci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = pick.format;
    ci.imageColorSpace = pick.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    CHECK(vkCreateSwapchainKHR(g.dev, &ci, nullptr, &sw.sc));
    vkGetSwapchainImagesKHR(g.dev, sw.sc, &n, nullptr);
    sw.images.resize(n);
    vkGetSwapchainImagesKHR(g.dev, sw.sc, &n, sw.images.data());
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (!sw.acquired) vkCreateSemaphore(g.dev, &si, nullptr, &sw.acquired);
    sw.done.resize(n);
    for (auto& s : sw.done) vkCreateSemaphore(g.dev, &si, nullptr, &s);
    sw.format = pick.format, sw.extent = extent, sw.srgb = srgb, sw.stale = false;
    static int logged;
    if (logged++ < 4)
        hos_log("vk: swapchain %ux%u, format %d, %u images, %s", extent.width, extent.height, (int)pick.format, n,
                mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo");
    return true;
}

bool srgb_format(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

// ---- HOS_SCREENSHOT ----
struct Shot {
    std::string path;
    int64_t at = -1;   // (-1: not read yet, -2: none or done)
} shot;

void write_bmp(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        hos_log("vk: screenshot %s cannot be written", path);
        return;
    }
    const uint32_t row = (w * 3 + 3) & ~3u, data = row * h, file = 54 + data;
    uint8_t hd[54] = {'B', 'M'};
    auto put32 = [&](int at, uint32_t v) { memcpy(hd + at, &v, 4); };
    put32(2, file), put32(10, 54), put32(14, 40), put32(18, w), put32(22, h);
    hd[26] = 1, hd[28] = 24;
    put32(34, data);
    fwrite(hd, 1, 54, f);
    std::vector<uint8_t> line(row, 0);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* s = rgba + (size_t)(h - 1 - y) * w * 4;
        for (uint32_t x = 0; x < w; x++) line[x * 3] = s[x * 4 + 2], line[x * 3 + 1] = s[x * 4 + 1], line[x * 3 + 2] = s[x * 4];
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
    hos_log("vk: screenshot %s (%ux%u)", path, w, h);
}

void take_screenshot(VkImage image, VkFormat format, uint32_t w, uint32_t h);
}  // namespace
void dump_written_images(const char* dir);   // (vk_texture.cpp)
void texdump(VkCommandBuffer cb);
void save_rgba_bmp(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h) { write_bmp(path, rgba, w, h); }
void save_image_bmp(VkImage image, VkFormat format, uint32_t w, uint32_t h, const char* path) {
    const std::string keep = shot.path;
    shot.path = path;
    take_screenshot(image, format, w, h);
    shot.path = keep;
}
namespace {
void take_screenshot(VkImage image, VkFormat format, uint32_t w, uint32_t h) {
    // (blitted to RGBA8 of the same encoding first: the window texture may be RGB10A2 or RGBA16F)
    const VkFormat out = srgb_format(format) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = out;
    ii.extent = {w, h, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage tmp;
    if (vkCreateImage(g.dev, &ii, nullptr, &tmp) != VK_SUCCESS) return;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.dev, tmp, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory mem;
    vkAllocateMemory(g.dev, &ai, nullptr, &mem);
    vkBindImageMemory(g.dev, tmp, mem, 0);
    HostBuffer hb = host_buffer((uint64_t)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkCommandBuffer cb = begin_commands();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = tmp;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    VkImageBlit bl{};
    bl.srcSubresource = bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[1] = bl.dstOffsets[1] = {(int32_t)w, (int32_t)h, 1};
    vkCmdBlitImage(cb, image, VK_IMAGE_LAYOUT_GENERAL, tmp, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
    full_barrier(cb);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cb, tmp, VK_IMAGE_LAYOUT_GENERAL, hb.buf, 1, &c);
    full_barrier(cb);
    end_and_wait(cb);
    write_bmp(shot.path.c_str(), hb.map, w, h);
    free_host_buffer(hb);
    vkDestroyImage(g.dev, tmp, nullptr);
    vkFreeMemory(g.dev, mem, nullptr);
}

void maybe_screenshot(VkImage image, VkFormat format, uint32_t w, uint32_t h);
/// HOS_VK_SHOTS=<dir>,<seconds> (debugging): the presented picture every that many seconds, <dir>/shot_<seconds>.bmp
void periodic_shots(VkImage image, VkFormat format, uint32_t w, uint32_t h) {
    static std::string dir;
    static double every = -1;
    static int64_t next = 0;
    if (every < 0) {
        every = 0;
        if (const char* e = getenv("HOS_VK_SHOTS")) {
            dir = e;
            const size_t comma = dir.rfind(',');
            every = comma == std::string::npos ? 5 : atof(dir.c_str() + comma + 1);
            if (comma != std::string::npos) dir.resize(comma);
            next = (int64_t)(every * 1e9);
        }
    }
    if (every <= 0 || hos_now_ns() < next) return;
    char path[512];
    snprintf(path, sizeof path, "%s/shot_%03d.bmp", dir.c_str(), (int)(next / 1000000000));
    next += (int64_t)(every * 1e9);
    const std::string keep = shot.path;
    shot.path = path;
    take_screenshot(image, format, w, h);
    shot.path = keep;
}
void maybe_screenshot(VkImage image, VkFormat format, uint32_t w, uint32_t h) {
    periodic_shots(image, format, w, h);
    if (shot.at == -1) {
        shot.at = -2;
        const char* s = getenv("HOS_SCREENSHOT");
        if (s && *s) {
            shot.path = s;
            double secs = 10;
            const size_t comma = shot.path.rfind(',');
            if (comma != std::string::npos) secs = atof(shot.path.c_str() + comma + 1), shot.path.resize(comma);
            shot.at = (int64_t)(secs * 1e9);
        }
    }
    if (shot.at < 0 || hos_now_ns() < shot.at) return;
    shot.at = -2;
    take_screenshot(image, format, w, h);
    if (const char* dir = getenv("HOS_VK_DUMP")) dump_written_images(dir);   // (debugging: every image the GPU wrote)
    texdump(VK_NULL_HANDLE);
}

}  // namespace

uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g.mem.memoryTypeCount; i++)
        if ((bits >> i & 1) && (g.mem.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < g.mem.memoryTypeCount; i++)
        if (bits >> i & 1) return i;
    return 0;
}

HostBuffer host_buffer(uint64_t size, VkBufferUsageFlags usage) {
    HostBuffer b;
    b.size = size;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    vkCreateBuffer(g.dev, &bi, nullptr, &b.buf);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g.dev, b.buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(g.dev, &ai, nullptr, &b.mem);
    vkBindBufferMemory(g.dev, b.buf, b.mem, 0);
    vkMapMemory(g.dev, b.mem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&b.map));
    return b;
}

void free_host_buffer(HostBuffer& b) {
    if (b.buf) vkDestroyBuffer(g.dev, b.buf, nullptr);
    if (b.mem) vkFreeMemory(g.dev, b.mem, nullptr);
    b = {};
}

VkCommandBuffer begin_commands() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(g.dev, &ai, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

void end_and_wait(VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    static VkFence fence = VK_NULL_HANDLE;
    if (!fence) {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(g.dev, &fi, nullptr, &fence);
    }
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    const VkResult r = vkQueueSubmit(g.queue, 1, &si, fence);
    if (r != VK_SUCCESS) hos_log("vk: vkQueueSubmit: %d", (int)r);
    else if (const VkResult w = vkWaitForFences(g.dev, 1, &fence, VK_TRUE, 10'000'000'000ull); w != VK_SUCCESS) {
        hos_log("vk: a submit did not finish (%d): the device is lost or hung", (int)w);
        abort();
    }
    vkResetFences(g.dev, 1, &fence);
    vkFreeCommandBuffers(g.dev, g.pool, 1, &cb);
}

void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0,
                         nullptr, 0, nullptr);
}

void present_image(VkImage image, VkFormat format, uint32_t w, uint32_t h, const int crop[4]) {
    maybe_screenshot(image, format, w, h);
    if (!g.surface) return;
    const bool srgb = srgb_format(format);
    if (!sw.sc || sw.stale || sw.srgb != srgb)
        if (!make_swapchain(srgb)) return;
    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(g.dev, sw.sc, 1'000'000'000ull, sw.acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        sw.stale = true;
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;
    VkCommandBuffer cb = begin_commands();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = sw.images[index];
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    VkClearColorValue black{};
    vkCmdClearColorImage(cb, sw.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &b.subresourceRange);
    b.oldLayout = b.newLayout;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    // the crop (x, y, w, h; w 0: the whole texture), scaled into the window keeping its shape
    int32_t sx = crop ? crop[0] : 0, sy = crop ? crop[1] : 0, cw = crop && crop[2] > 0 ? crop[2] : (int32_t)w,
            ch = crop && crop[3] > 0 ? crop[3] : (int32_t)h;
    const double scale = std::min((double)sw.extent.width / cw, (double)sw.extent.height / ch);
    const int32_t dw = (int32_t)(cw * scale), dh = (int32_t)(ch * scale);
    const int32_t dx = ((int32_t)sw.extent.width - dw) / 2, dy = ((int32_t)sw.extent.height - dh) / 2;
    VkImageBlit bl{};
    bl.srcSubresource = bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[0] = {sx, sy, 0}, bl.srcOffsets[1] = {sx + cw, sy + ch, 1};
    bl.dstOffsets[0] = {dx, dy, 0}, bl.dstOffsets[1] = {dx + dw, dy + dh, 1};
    vkCmdBlitImage(cb, image, VK_IMAGE_LAYOUT_GENERAL, sw.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                   VK_FILTER_LINEAR);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, b.dstAccessMask = 0;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1, si.pWaitSemaphores = &sw.acquired, si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1, si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1, si.pSignalSemaphores = &sw.done[index];
    vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1, pi.pWaitSemaphores = &sw.done[index];
    pi.swapchainCount = 1, pi.pSwapchains = &sw.sc, pi.pImageIndices = &index;
    r = vkQueuePresentKHR(g.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) sw.stale = true;
    vkQueueWaitIdle(g.queue);   // (the command buffer and the semaphores are reused: simple, and the frame is done)
    vkFreeCommandBuffers(g.dev, g.pool, 1, &cb);
}

namespace {

bool init(SDL_Window* window) {
    g.window = window;
    // ---- instance
    std::vector<const char*> exts;
    if (window) {
        Uint32 n = 0;
        const char* const* e = SDL_Vulkan_GetInstanceExtensions(&n);
        if (!e) {
            hos_log("vk: SDL has no Vulkan surface extensions (%s)", SDL_GetError());
            return false;
        }
        exts.assign(e, e + n);
    }
    std::vector<const char*> layers;
    if (env_on("HOS_VK_VALIDATE")) layers.push_back("VK_LAYER_KHRONOS_validation");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "oranges";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (uint32_t)exts.size(), ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = (uint32_t)layers.size(), ici.ppEnabledLayerNames = layers.data();
    CHECK(vkCreateInstance(&ici, nullptr, &g.instance));
    if (window && !SDL_Vulkan_CreateSurface(window, g.instance, nullptr, &g.surface)) {
        hos_log("vk: no surface (%s)", SDL_GetError());
        return false;
    }
    // ---- physical device: HOS_VK_DEVICE, else the first discrete GPU with what the back end needs
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(g.instance, &n, devs.data());
    static const char* const required[] = {VK_EXT_SHADER_OBJECT_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
                                           VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
    int best = -1, best_score = -1;
    const char* want = getenv("HOS_VK_DEVICE");
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(devs[i], nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> e(ne);
        vkEnumerateDeviceExtensionProperties(devs[i], nullptr, &ne, e.data());
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        bool ok = p.apiVersion >= VK_API_VERSION_1_3 && (!window || has_ext(e, VK_KHR_SWAPCHAIN_EXTENSION_NAME));
        for (const char* r : required) ok = ok && has_ext(e, r);
        if (!ok) {
            hos_log("vk: %s lacks what the back end needs (Vulkan 1.3, shader objects, push descriptors, host memory)",
                    p.deviceName);
            continue;
        }
        const int score = want && *want ? (atoi(want) == (int)i ? 100 : 0)
                                        : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 10
                                        : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 5 : 1;
        if (score > best_score) best = (int)i, best_score = score;
    }
    if (best < 0) {
        hos_log("vk: no usable device");
        return false;
    }
    g.phys = devs[best];
    vkGetPhysicalDeviceProperties(g.phys, &g.props);
    vkGetPhysicalDeviceMemoryProperties(g.phys, &g.mem);
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &ne, nullptr);
    std::vector<VkExtensionProperties> dev_exts(ne);
    vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &ne, dev_exts.data());
    // ---- queue family: graphics and compute (and present)
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &n, nullptr);
    std::vector<VkQueueFamilyProperties> fams(n);
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &n, fams.data());
    g.family = ~0u;
    for (uint32_t i = 0; i < n && g.family == ~0u; i++) {
        VkBool32 present = VK_TRUE;
        if (g.surface) vkGetPhysicalDeviceSurfaceSupportKHR(g.phys, i, g.surface, &present);
        if ((fams[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT) && present)
            g.family = i;
    }
    if (g.family == ~0u) {
        hos_log("vk: no graphics queue");
        return false;
    }
    // ---- features: every core feature the device has, and the extensions' (eden's SPIR-V uses many)
    VkPhysicalDeviceShaderObjectFeaturesEXT so{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vid{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceDepthClipControlFeaturesEXT dcc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    VkPhysicalDeviceDepthClipEnableFeaturesEXT dce{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesEXT rb2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceImage2DViewOf3DFeaturesEXT v3d{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    std::vector<const char*> dexts;
    if (window) dexts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    for (const char* r : required) dexts.push_back(r);
    void* chain = nullptr;
    auto add = [&](const char* ext, auto& feat) {
        if (ext && !has_ext(dev_exts, ext)) return;
        if (ext) dexts.push_back(ext);
        feat.pNext = chain;
        chain = &feat;
    };
    add(nullptr, f11), add(nullptr, f12), add(nullptr, f13);
    add(nullptr, so);   // (its extension is in `required`)
    add(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, eds3);
    add(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME, eds2);
    add(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME, vid);
    add(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, dcc);
    add(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME, dce);
    add(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, cbc);
    add(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, rb2);
    add(VK_EXT_IMAGE_2D_VIEW_OF_3D_EXTENSION_NAME, v3d);
    f2.pNext = chain;
    vkGetPhysicalDeviceFeatures2(g.phys, &f2);
    if (!so.shaderObject || !f13.dynamicRendering || !f13.synchronization2) {
        hos_log("vk: %s: no shader objects or dynamic rendering", g.props.deviceName);
        return false;
    }
    f2.features.robustBufferAccess = VK_FALSE;   // (what the game reads past a buffer's end is its own memory)
    rb2.robustBufferAccess2 = VK_FALSE, rb2.robustImageAccess2 = VK_FALSE;   // (nullDescriptor is what is wanted)
    f13.robustImageAccess = VK_FALSE;
    f12.bufferDeviceAddressCaptureReplay = f12.bufferDeviceAddressMultiDevice = VK_FALSE;
    f11.protectedMemory = VK_FALSE;
    g.tessellation = f2.features.tessellationShader, g.geometry = f2.features.geometryShader;
    g.custom_border = cbc.customBorderColors && cbc.customBorderColorWithoutFormat;
    g.view_2d_of_3d = v3d.image2DViewOf3D && v3d.sampler2DViewOf3D;
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = g.family, qi.queueCount = 1, qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1, dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = (uint32_t)dexts.size(), dci.ppEnabledExtensionNames = dexts.data();
    CHECK(vkCreateDevice(g.phys, &dci, nullptr, &g.dev));
    vkGetDeviceQueue(g.dev, g.family, 0, &g.queue);
#define OVK_LOAD(f) g.x.f = reinterpret_cast<PFN_##f>(vkGetDeviceProcAddr(g.dev, #f));
    OVK_EXT_FUNCS(OVK_LOAD)
#undef OVK_LOAD
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &hp;
    vkGetPhysicalDeviceProperties2(g.phys, &p2);
    g.host_align = hp.minImportedHostPointerAlignment ? hp.minImportedHostPointerAlignment : 4096;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g.family;
    CHECK(vkCreateCommandPool(g.dev, &pci, nullptr, &g.pool));
    hos_log("vk: %s (Vulkan %u.%u.%u, driver %#x)%s", g.props.deviceName, VK_API_VERSION_MAJOR(g.props.apiVersion),
            VK_API_VERSION_MINOR(g.props.apiVersion), VK_API_VERSION_PATCH(g.props.apiVersion), g.props.driverVersion,
            window ? "" : ", headless");
    return true;
}

}  // namespace
}  // namespace ovk

/// The back end's start, on the process's main thread (window: the SDL window, null headless). 0: the null back end.
extern "C" int rx_vk_init(void* window) {
    const char* s = getenv("HOS_VK");
    if (s && !strcmp(s, "0")) {
        hos_log("vk: off (HOS_VK=0): nothing is drawn");
        return 0;
    }
    std::lock_guard lk(ovk::big_lock);
    if (!ovk::init(static_cast<SDL_Window*>(window))) {
        hos_log("vk: the Vulkan back end did not start: nothing is drawn");
        if (ovk::g.dev) vkDestroyDevice(ovk::g.dev, nullptr);
        if (ovk::g.surface) vkDestroySurfaceKHR(ovk::g.instance, ovk::g.surface, nullptr);
        if (ovk::g.instance) vkDestroyInstance(ovk::g.instance, nullptr);
        ovk::g = {};
        return 0;
    }
    return 1;
}
