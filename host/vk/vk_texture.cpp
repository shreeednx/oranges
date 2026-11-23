// Textures and samplers: NVN textures as Vulkan images, the texture and sampler pools' registrations, samplers.
//
// An image is made at a texture's first use, keyed by its storage address and description, and filled from guest
// memory: the Tegra block-linear layout (host/nvn_layout.c: levels, block heights, layer size) is deswizzled on the CPU
// into a staging buffer. One the GPU has written (a render target, a copy or clear destination) is never filled from
// guest memory again. Others are checked once a frame with a fingerprint of their storage (a few kilobytes of it), and
// every few frames by a hash of all of it (32 MB a frame at most), and filled again when it changed: the game loads
// textures into memory it reuses, and writes its font's glyphs into a texture as text needs them.
//
// Formats: BCn as they are; ASTC, which desktop GPUs do not sample, decoded to RGBA8 on the CPU (suyu's decoder,
// third_party/eden/video_core/textures); RGB (3-component) and 5-bit formats are left out. Views follow the type the shader declares (a 2D array texture sampled as 2D gets a
// 2D view of its first layer), with the registration's levels, layers, format and swizzle.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <span>

#include "shader_recompiler/shader_info.h"
#include "video_core/textures/astc.h"
#include "vk.h"

namespace ovk {

struct Image {
    nvn_texdesc d{};
    uint64_t gpu = 0;
    nvn_texlayout L{};
    FormatInfo fi;
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageType type = VK_IMAGE_TYPE_2D;
    uint32_t w = 1, h = 1, depth = 1, layers = 1, levels = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags usage = 0;
    bool written = false, filled = false;
    uint64_t fingerprint = 0, checked_frame = ~0ull;
    uint64_t content = 0, content_frame = 0;   // (a hash of all of the storage, and the frame it was taken)
    std::unordered_map<uint64_t, VkImageView> views;
};

void staging_release();
void save_image_bmp(VkImage image, VkFormat format, uint32_t w, uint32_t h, const char* path);

namespace {

uint64_t frame = 0;   // (presents: images are checked against guest memory once a frame)

uint64_t hash_bytes(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

// ---- formats ----
/// the format a storage view of an image takes: the linear twin of an sRGB format (sRGB cannot be stored)
VkFormat storage_twin(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    default: return f;
    }
}

struct FormatRow {
    uint16_t nvn;
    VkFormat vk;
    uint8_t kind;   // 0 float/unorm/snorm, 1 uint, 2 sint, 3 depth, 4 depth+stencil, 5 stencil
};
const FormatRow kFormats[] = {
    {0x01, VK_FORMAT_R8_UNORM, 0}, {0x02, VK_FORMAT_R8_SNORM, 0}, {0x03, VK_FORMAT_R8_UINT, 1}, {0x04, VK_FORMAT_R8_SINT, 2},
    {0x05, VK_FORMAT_R16_SFLOAT, 0}, {0x06, VK_FORMAT_R16_UNORM, 0}, {0x07, VK_FORMAT_R16_SNORM, 0},
    {0x08, VK_FORMAT_R16_UINT, 1}, {0x09, VK_FORMAT_R16_SINT, 2}, {0x0A, VK_FORMAT_R32_SFLOAT, 0},
    {0x0B, VK_FORMAT_R32_UINT, 1}, {0x0C, VK_FORMAT_R32_SINT, 2}, {0x0D, VK_FORMAT_R8G8_UNORM, 0},
    {0x0E, VK_FORMAT_R8G8_SNORM, 0}, {0x0F, VK_FORMAT_R8G8_UINT, 1}, {0x10, VK_FORMAT_R8G8_SINT, 2},
    {0x11, VK_FORMAT_R16G16_SFLOAT, 0}, {0x12, VK_FORMAT_R16G16_UNORM, 0}, {0x13, VK_FORMAT_R16G16_SNORM, 0},
    {0x14, VK_FORMAT_R16G16_UINT, 1}, {0x15, VK_FORMAT_R16G16_SINT, 2}, {0x16, VK_FORMAT_R32G32_SFLOAT, 0},
    {0x17, VK_FORMAT_R32G32_UINT, 1}, {0x18, VK_FORMAT_R32G32_SINT, 2}, {0x25, VK_FORMAT_R8G8B8A8_UNORM, 0},
    {0x26, VK_FORMAT_R8G8B8A8_SNORM, 0}, {0x27, VK_FORMAT_R8G8B8A8_UINT, 1}, {0x28, VK_FORMAT_R8G8B8A8_SINT, 2},
    {0x29, VK_FORMAT_R16G16B16A16_SFLOAT, 0}, {0x2A, VK_FORMAT_R16G16B16A16_UNORM, 0},
    {0x2B, VK_FORMAT_R16G16B16A16_SNORM, 0}, {0x2C, VK_FORMAT_R16G16B16A16_UINT, 1},
    {0x2D, VK_FORMAT_R16G16B16A16_SINT, 2}, {0x2E, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
    {0x2F, VK_FORMAT_R32G32B32A32_UINT, 1}, {0x30, VK_FORMAT_R32G32B32A32_SINT, 2}, {0x31, VK_FORMAT_S8_UINT, 5},
    {0x32, VK_FORMAT_D16_UNORM, 3}, {0x33, VK_FORMAT_X8_D24_UNORM_PACK32, 3}, {0x34, VK_FORMAT_D32_SFLOAT, 3},
    {0x35, VK_FORMAT_D24_UNORM_S8_UINT, 4}, {0x36, VK_FORMAT_D32_SFLOAT_S8_UINT, 4},
    {0x37, VK_FORMAT_R8G8B8A8_SRGB, 0}, {0x38, VK_FORMAT_R8G8B8A8_SRGB, 0}, {0x39, VK_FORMAT_A4B4G4R4_UNORM_PACK16, 0},
    {0x3C, VK_FORMAT_B5G6R5_UNORM_PACK16, 0}, {0x3D, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 0},
    {0x3E, VK_FORMAT_A2B10G10R10_UINT_PACK32, 1}, {0x3F, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 0},
    {0x40, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 0}, {0x41, VK_FORMAT_BC1_RGB_UNORM_BLOCK, 0},
    {0x42, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 0}, {0x43, VK_FORMAT_BC2_UNORM_BLOCK, 0}, {0x44, VK_FORMAT_BC3_UNORM_BLOCK, 0},
    {0x45, VK_FORMAT_BC1_RGB_SRGB_BLOCK, 0}, {0x46, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 0}, {0x47, VK_FORMAT_BC2_SRGB_BLOCK, 0},
    {0x48, VK_FORMAT_BC3_SRGB_BLOCK, 0}, {0x49, VK_FORMAT_BC4_UNORM_BLOCK, 0}, {0x4A, VK_FORMAT_BC4_SNORM_BLOCK, 0},
    {0x4B, VK_FORMAT_BC5_UNORM_BLOCK, 0}, {0x4C, VK_FORMAT_BC5_SNORM_BLOCK, 0}, {0x4D, VK_FORMAT_BC7_UNORM_BLOCK, 0},
    {0x4E, VK_FORMAT_BC7_SRGB_BLOCK, 0}, {0x4F, VK_FORMAT_BC6H_SFLOAT_BLOCK, 0}, {0x50, VK_FORMAT_BC6H_UFLOAT_BLOCK, 0},
    {0x69, VK_FORMAT_A2B10G10R10_SNORM_PACK32, 0}, {0x6A, VK_FORMAT_A2B10G10R10_SINT_PACK32, 2},
    {0x6D, VK_FORMAT_R8G8B8A8_UNORM, 0}, {0x6E, VK_FORMAT_R8G8B8A8_SNORM, 0}, {0x6F, VK_FORMAT_R8G8B8A8_UINT, 1},
    {0x70, VK_FORMAT_R8G8B8A8_SINT, 2}, {0x71, VK_FORMAT_R16G16B16A16_SFLOAT, 0}, {0x72, VK_FORMAT_R16G16B16A16_UNORM, 0},
    {0x73, VK_FORMAT_R16G16B16A16_SNORM, 0}, {0x74, VK_FORMAT_R16G16B16A16_UINT, 1}, {0x75, VK_FORMAT_R16G16B16A16_SINT, 2},
    {0x76, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {0x77, VK_FORMAT_R32G32B32A32_UINT, 1},
    {0x78, VK_FORMAT_R32G32B32A32_SINT, 2}, {0x95, VK_FORMAT_R5G6B5_UNORM_PACK16, 0},
    {0x99, VK_FORMAT_B8G8R8A8_UNORM, 0}, {0x9A, VK_FORMAT_B8G8R8A8_UNORM, 0}, {0x9B, VK_FORMAT_B8G8R8A8_SRGB, 0},
    {0x9C, VK_FORMAT_B8G8R8A8_SRGB, 0},
};

bool is_rgbx(int f) { return f == 0x37 || (f >= 0x6D && f <= 0x78) || f == 0x99 || f == 0x9B; }
bool is_astc(int f) { return f >= 0x79 && f <= 0x94; }

VkFormatFeatureFlags features_of(VkFormat f) {
    static std::unordered_map<int, VkFormatFeatureFlags> cache;
    auto it = cache.find((int)f);
    if (it != cache.end()) return it->second;
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g.phys, f, &p);
    return cache[(int)f] = p.optimalTilingFeatures;
}

VkComponentSwizzle swizzle_of(int s) {
    switch (s) {
    case 0: return VK_COMPONENT_SWIZZLE_ZERO;
    case 1: return VK_COMPONENT_SWIZZLE_ONE;
    case 2: return VK_COMPONENT_SWIZZLE_R;
    case 3: return VK_COMPONENT_SWIZZLE_G;
    case 4: return VK_COMPONENT_SWIZZLE_B;
    default: return VK_COMPONENT_SWIZZLE_A;
    }
}

// ---- block linear ----
inline uint32_t gob_offset(uint32_t x, uint32_t y) {   // (x in bytes within the GOB's 64, y within its 8 rows)
    return ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16);
}

/// one level of one layer: rows of `row` bytes, `rows` of them, `slices` deep, from block-linear `src`
void deswizzle(const uint8_t* src, uint64_t src_size, uint8_t* dst, uint32_t row, uint32_t rows, uint32_t slices,
               unsigned tile_h, unsigned tile_d) {
    const uint32_t gobs_x = (row + 63) / 64, bh = 1u << tile_h, bd = 1u << tile_d;
    const uint32_t blocks_y = (rows + 8 * bh - 1) / (8 * bh);
    const uint64_t block_bytes = 512ull << (tile_h + tile_d);
    for (uint32_t z = 0; z < slices; z++)
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t* out = dst + ((uint64_t)z * rows + y) * row;
            const uint64_t block_row = ((uint64_t)(z / bd) * blocks_y + y / (8 * bh)) * gobs_x;
            const uint64_t in_block = ((uint64_t)(z % bd) * bh + (y / 8) % bh) * 512;
            for (uint32_t x = 0; x < row; x += 16) {
                const uint64_t off = (block_row + x / 64) * block_bytes + in_block + gob_offset(x, y);
                const uint32_t n = std::min(16u, row - x);
                if (off + n <= src_size) memcpy(out + x, src + off, n);
                else memset(out + x, 0, n);
            }
        }
}

uint32_t mip(uint32_t s, uint32_t l) { return std::max(1u, s >> l); }

// ---- images ----
std::unordered_map<uint64_t, Image*> images;

uint64_t image_key(const nvn_tex* t) {
    struct {
        uint64_t gpu;
        int target, format, flags, samples;
        uint32_t w, h, d, levels, stride;
        int pkg;
    } k{t->gpu, t->d.target, t->d.format, t->d.flags & 0x50, t->d.samples, t->d.width, t->d.height, t->d.depth,
        t->d.levels, t->d.stride, t->d.pkg_block_h};
    return hash_bytes(&k, sizeof k);
}

bool create(Image* im) {
    const nvn_texdesc& d = im->d;
    if (!im->fi.format) return false;
    im->levels = std::max(1u, d.levels);
    im->w = std::max(1u, d.width), im->h = std::max(1u, d.height), im->depth = 1, im->layers = 1;
    VkImageCreateFlags flags = 0;
    switch (d.target) {
    case NVN_TEXTURE_TARGET_1D: im->type = VK_IMAGE_TYPE_1D, im->h = 1; break;
    case NVN_TEXTURE_TARGET_1D_ARRAY: im->type = VK_IMAGE_TYPE_1D, im->layers = im->h, im->h = 1; break;
    case NVN_TEXTURE_TARGET_3D: im->type = VK_IMAGE_TYPE_3D, im->depth = std::max(1u, d.depth); break;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY:
        im->layers = std::max(1u, d.depth);
        break;
    case NVN_TEXTURE_TARGET_CUBEMAP: im->layers = 6, flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY:
        im->layers = std::max(6u, d.depth), flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        break;
    case NVN_TEXTURE_TARGET_BUFFER: return false;
    default: break;
    }
    if (im->type == VK_IMAGE_TYPE_2D && im->layers >= 6 && im->w == im->h) flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    im->samples = d.samples >= 8 ? VK_SAMPLE_COUNT_8_BIT : d.samples >= 4 ? VK_SAMPLE_COUNT_4_BIT
                  : d.samples >= 2 ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_1_BIT;
    const VkFormatFeatureFlags ff = features_of(im->fi.format);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    const bool mutable_format = !im->fi.compressed && !im->fi.depth && !im->fi.stencil;
    if (mutable_format) flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    // (storage: sRGB images through their linear twin's views, which the extended usage allows)
    const VkFormat st = mutable_format ? storage_twin(im->fi.format) : im->fi.format;
    if ((features_of(st) & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && im->samples == VK_SAMPLE_COUNT_1_BIT)
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    im->usage = usage;
    if (im->type == VK_IMAGE_TYPE_3D) {   // (its slices as render targets, and as 2D textures)
        flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        if (g.view_2d_of_3d) flags |= VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT;
    }
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = flags;
    ci.imageType = im->type;
    ci.format = im->fi.format;
    ci.extent = {im->w, im->h, im->depth};
    // (levels past what the size has: NVN allows them, Vulkan does not)
    const uint32_t biggest = std::max({im->w, im->h, im->depth});
    uint32_t most = 1;
    while ((biggest >> most) > 0) most++;
    im->levels = std::min(im->levels, most);
    if (im->samples != VK_SAMPLE_COUNT_1_BIT) im->levels = 1;
    ci.mipLevels = im->levels;
    ci.arrayLayers = im->layers;
    ci.samples = im->samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g.dev, &ci, nullptr, &im->img) != VK_SUCCESS) {
        vk_log_limited("image", 16, "vk: no image for texture format %#x %ux%ux%u target %d", d.format, d.width, d.height,
                       d.depth, d.target);
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.dev, im->img, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g.dev, &ai, nullptr, &im->mem) != VK_SUCCESS) {
        vkDestroyImage(g.dev, im->img, nullptr);
        im->img = VK_NULL_HANDLE;
        return false;
    }
    vkBindImageMemory(g.dev, im->img, im->mem, 0);
    return true;
}

void to_general(VkCommandBuffer cb, Image* im) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im->img;
    b.subresourceRange = {(VkImageAspectFlags)(im->fi.depth || im->fi.stencil
                                                   ? (im->fi.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                                         (im->fi.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)
                                                   : VK_IMAGE_ASPECT_COLOR_BIT),
                          0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}

/// a hash of n bytes, four 64-bit lanes at a time (memory speed: the whole of a texture's storage is hashed)
uint64_t fast_hash(const uint8_t* p, uint64_t n) {
    constexpr uint64_t k = 0x9E3779B97F4A7C15ull;
    uint64_t h[4] = {n, k, ~n, k ^ n};
    uint64_t i = 0;
    for (; i + 32 <= n; i += 32)
        for (int l = 0; l < 4; l++) {
            uint64_t v;
            memcpy(&v, p + i + 8 * l, 8);
            h[l] = (h[l] ^ v) * k;
            h[l] ^= h[l] >> 29;
        }
    uint64_t r = h[0] ^ (h[1] << 1) ^ (h[2] << 2) ^ (h[3] << 3);
    for (; i < n; i++) r = (r ^ p[i]) * 0x100000001b3ull;
    return r;
}

/// all of an image's storage hashed (0: not contiguous host memory)
uint64_t content_hash(const Image* im) {
    const uint8_t* p = nvn_gpu_host(im->gpu);
    if (!p || nvn_gpu_contiguous(im->gpu, im->L.size) < im->L.size) return 0;
    return fast_hash(p, im->L.size);
}
uint64_t hash_budget = 0;   // (bytes of storage still to hash this frame)
constexpr uint64_t kHashBudget = 32ull << 20;
constexpr uint64_t kHashEvery = 4;   // (frames between an image's whole-storage checks, budget allowing)

/// a fingerprint of the storage: all of it when small, else 64 spans of 64 bytes spread over it
uint64_t fingerprint(const Image* im) {
    const uint64_t size = im->L.size;
    const uint8_t* p = nvn_gpu_host(im->gpu);
    if (!p || nvn_gpu_contiguous(im->gpu, size) < size) return 0;
    if (size <= 4096) return hash_bytes(p, size);
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 64; i++) h = hash_bytes(p + (size - 64) * i / 63, 64, h);
    return h;
}

// (staging for fills: kept, grown as needed; the fill's command buffer is run before the next fill reuses it)
std::vector<HostBuffer> staging_used;
HostBuffer staging(uint64_t size) {
    HostBuffer b = host_buffer(std::max<uint64_t>(size, 1), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    staging_used.push_back(b);
    return b;
}

void fill(VkCommandBuffer cb, Image* im) {
    im->filled = true;
    const nvn_texdesc& d = im->d;
    if (im->samples != VK_SAMPLE_COUNT_1_BIT || !im->L.size) return;
    int bw = 1, bh = 1;
    const int bpb = nvn_fmt_bpb(d.format, &bw, &bh);
    if (bpb <= 0) return;
    // the storage, as host memory (copied when it is not contiguous: a virtual pool)
    std::vector<uint8_t> copy;
    const uint8_t* src = nvn_gpu_host(im->gpu);
    if (!src || nvn_gpu_contiguous(im->gpu, im->L.size) < im->L.size) {
        copy.resize(im->L.size);
        if (!nvn_gpu_read(im->gpu, copy.data(), im->L.size)) return;
        src = copy.data();
    }
    // the staging buffer: every level of every layer, tightly packed
    struct Part {
        uint32_t level, layer, w, h, depth;
        uint64_t at;
    };
    std::vector<Part> parts;
    uint64_t total = 0;
    const bool astc = im->fi.astc;
    for (uint32_t layer = 0; layer < im->layers; layer++)
        for (uint32_t l = 0; l < im->levels; l++) {
            const uint32_t w = mip(im->w, l), h = mip(im->h, l), z = im->type == VK_IMAGE_TYPE_3D ? mip(im->depth, l) : 1;
            const uint64_t bytes = astc ? (uint64_t)w * h * z * 4
                                        : (uint64_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * z * bpb;
            parts.push_back({l, layer, w, h, z, total});
            total += (bytes + 15) & ~15ull;
        }
    HostBuffer st = staging(total);
    std::vector<VkBufferImageCopy> copies;
    for (const Part& p : parts) {
        uint8_t* out = st.map + p.at;
        if (astc) {   // (the blocks deswizzled, then decoded to RGBA8 by suyu's decoder: desktop GPUs sample no ASTC)
            const uint32_t row = (p.w + bw - 1) / bw * bpb, rows = (p.h + bh - 1) / bh;
            const uint64_t base = (uint64_t)p.layer * im->L.layer_size + nvn_tex_level_offset(&d, p.level);
            if (base >= im->L.size) continue;
            std::vector<uint8_t> blocks((size_t)row * rows * p.depth);
            unsigned th = 0, td = 0;
            nvn_tex_level_tiles(&d, p.level, &th, &td);
            deswizzle(src + base, im->L.size - base, blocks.data(), row, rows, p.depth, th, td);
            Tegra::Texture::ASTC::DecompressSerial(std::span<const uint8_t>(blocks), p.w, p.h, p.depth, (uint32_t)bw,
                                                   (uint32_t)bh, std::span<uint8_t>(out, (size_t)p.w * p.h * p.depth * 4));
        } else {
            const uint32_t row = (p.w + bw - 1) / bw * bpb, rows = (p.h + bh - 1) / bh;
            if (nvn_tex_is_linear(&d)) {
                const uint32_t stride = nvn_tex_linear_stride(&d);
                for (uint32_t z = 0; z < p.depth; z++)
                    for (uint32_t y = 0; y < rows; y++) {
                        const uint64_t off = (uint64_t)y * stride;
                        if (off + row <= im->L.size) memcpy(out + ((uint64_t)z * rows + y) * row, src + off, row);
                    }
            } else {
                const uint64_t base = (uint64_t)p.layer * im->L.layer_size + nvn_tex_level_offset(&d, p.level);
                if (base >= im->L.size) continue;
                unsigned th = 0, td = 0;
                nvn_tex_level_tiles(&d, p.level, &th, &td);
                deswizzle(src + base, im->L.size - base, out, row, rows, p.depth, th, td);
            }
        }
        VkBufferImageCopy c{};
        c.bufferOffset = p.at;
        c.imageSubresource = {(VkImageAspectFlags)(im->fi.depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                    : im->fi.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
                              p.level, p.layer, 1};
        c.imageExtent = {p.w, p.h, p.depth};
        copies.push_back(c);
    }
    if (im->fi.depth && im->fi.stencil) return;   // (packed depth-stencil: not filled from memory)
    full_barrier(cb);
    vkCmdCopyBufferToImage(cb, st.buf, im->img, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)copies.size(), copies.data());
    full_barrier(cb);
}

}  // namespace

FormatInfo format_info(int f) {
    FormatInfo fi;
    if (is_astc(f)) {
        const bool srgb = f >= 0x87;
        fi.format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        fi.astc = true;
        return fi;
    }
    for (const FormatRow& r : kFormats)
        if (r.nvn == f) {
            fi.format = r.vk;
            fi.integer = r.kind == 1 || r.kind == 2;
            fi.sint = r.kind == 2;
            fi.depth = r.kind == 3 || r.kind == 4;
            fi.stencil = r.kind == 4 || r.kind == 5;
            fi.compressed = f >= 0x41 && f <= 0x50;
            break;
        }
    // (24-bit depth where the device has none: 32-bit float)
    if (fi.format == VK_FORMAT_X8_D24_UNORM_PACK32 && !(features_of(fi.format) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        fi.format = VK_FORMAT_D32_SFLOAT;
    if (fi.format == VK_FORMAT_D24_UNORM_S8_UINT && !(features_of(fi.format) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        fi.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    return fi;
}

void (*before_transfer)() = nullptr;

Image* image_find(const nvn_tex* t) {
    if (!t || !t->gpu) return nullptr;
    auto it = images.find(image_key(t));
    return it == images.end() || !it->second->img ? nullptr : it->second;
}

void image_dirty(Image* im) {   // (the CPU wrote its memory: filled again at its next use, even if the GPU wrote it)
    im->written = false, im->filled = false, im->checked_frame = ~0ull;
}

/// the images whose storage overlaps [gpu, gpu + size) the GPU has not written: filled again at their next use
int images_dirty(uint64_t gpu, uint64_t size) {
    int n = 0;
    for (auto& [key, im] : images)
        if (im->img && !im->written && im->gpu < gpu + size && gpu < im->gpu + im->L.size) {
            im->filled = false, im->checked_frame = ~0ull;
            n++;
        }
    return n;
}

Image* image_of(VkCommandBuffer cb, const nvn_tex* t) {
    if (!t || !t->gpu) return nullptr;
    const uint64_t key = image_key(t);
    auto it = images.find(key);
    Image* im = it == images.end() ? nullptr : it->second;
    if (!im) {
        if (before_transfer) before_transfer();
        im = new Image;
        im->d = t->d, im->gpu = t->gpu, im->L = t->L;
        im->fi = format_info(t->d.format);
        if (!create(im)) {
            if (im->fi.format)
                vk_log_limited("image", 16, "vk: texture format %#x target %d is not made", t->d.format, t->d.target);
            im->img = VK_NULL_HANDLE;
        }
        images[key] = im;
        if (im->img) to_general(cb, im);
    }
    if (!im->img) return nullptr;
    // Filled from guest memory again when the CPU changed it: a fingerprint every frame (cheap, a reused memory's new
    // texture), and every few frames, within a budget of bytes a frame, a hash of all of it. The game writes some
    // textures in place, a little at a time (its font's glyph cache: a glyph when text first needs it), with nothing
    // that says so (no nvnMemoryPoolFlushMappedRange: see rx_nvn_flushed).
    if (!im->written && im->checked_frame != frame) {
        im->checked_frame = frame;
        const uint64_t fp = fingerprint(im);
        bool changed = !im->filled || fp != im->fingerprint;
        if (!changed && frame - im->content_frame >= kHashEvery && hash_budget >= im->L.size) {
            hash_budget -= im->L.size;
            im->content_frame = frame;
            changed = content_hash(im) != im->content;
        }
        if (changed) {
            im->fingerprint = fp;
            im->content = content_hash(im), im->content_frame = frame;
            if (before_transfer) before_transfer();
            fill(cb, im);
        }
    }
    return im;
}

VkImage image_handle(Image* im) { return im->img; }
VkFormat image_format(Image* im) { return im->fi.format; }
const FormatInfo& image_format_info(Image* im) { return im->fi; }
void image_extent(Image* im, uint32_t level, uint32_t* w, uint32_t* h) {
    *w = mip(im->w, level), *h = mip(im->h, level);
}
uint32_t image_layers(Image* im) { return im->layers; }
VkSampleCountFlagBits image_samples(Image* im) { return im->samples; }
void image_written(Image* im) {
    im->written = true;
}

VkImageView view_of(Image* im, const nvn_view* v, bool has_view, int shader_type, int use) {
    const bool for_attachment = use == kViewAttachment, storage = use == kViewStorage;
    // levels, layers, format, swizzle: the view's where it sets them
    uint32_t base_level = 0, levels = im->levels, base_layer = 0, layers = im->layers;
    VkFormat format = im->fi.format;
    int sw[4] = {im->d.swizzle_r, im->d.swizzle_g, im->d.swizzle_b, im->d.swizzle_a};
    const bool tex_swizzle = sw[0] || sw[1] || sw[2] || sw[3];
    if (!tex_swizzle) sw[0] = 2, sw[1] = 3, sw[2] = 4, sw[3] = 5;
    if (has_view && v) {
        if (v->set_levels) base_level = std::min<uint32_t>(v->base_level, im->levels - 1), levels = v->nlevels;
        if (v->set_layers) base_layer = std::min<uint32_t>(v->min_layer, im->layers - 1), layers = v->nlayers;
        if (v->set_format) {
            const FormatInfo f = format_info(v->format);
            if (f.format && !f.compressed && !im->fi.compressed && !im->fi.depth && !f.depth) format = f.format;
        }
        if (v->set_swizzle) sw[0] = v->swz[0], sw[1] = v->swz[1], sw[2] = v->swz[2], sw[3] = v->swz[3];
    }
    if (storage) {   // (one level, no swizzle, and the sRGB formats' linear twins: sRGB cannot be stored)
        levels = 1, sw[0] = 2, sw[1] = 3, sw[2] = 4, sw[3] = 5;
        format = storage_twin(format);
    }
    levels = std::max(1u, std::min(levels, im->levels - base_level));
    layers = std::max(1u, std::min(layers, im->layers - base_layer));
    if (is_rgbx(im->d.format) && !for_attachment && !storage) sw[3] = 1;
    VkImageViewType type;
    if (for_attachment) {
        type = im->type == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D;
        levels = 1, layers = 1;
        if (im->type == VK_IMAGE_TYPE_3D)   // (a slice: the view's first layer, of the level's depth)
            base_layer = has_view && v && v->set_layers ? std::min<uint32_t>(v->min_layer, mip(im->depth, base_level) - 1) : 0;
    } else {
        switch ((Shader::TextureType)shader_type) {
        case Shader::TextureType::Color1D: type = VK_IMAGE_VIEW_TYPE_1D, layers = 1; break;
        case Shader::TextureType::ColorArray1D: type = VK_IMAGE_VIEW_TYPE_1D_ARRAY; break;
        case Shader::TextureType::Color3D: type = VK_IMAGE_VIEW_TYPE_3D, layers = 1; break;
        case Shader::TextureType::ColorArray2D: type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
        case Shader::TextureType::ColorCube: type = VK_IMAGE_VIEW_TYPE_CUBE, layers = 6; break;
        case Shader::TextureType::ColorArrayCube: type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, layers = layers / 6 * 6; break;
        default: type = VK_IMAGE_VIEW_TYPE_2D, layers = 1; break;
        }
        // (a type the image cannot be seen as: a 3D image as anything but 3D or (its first slice) 2D, a non-cube image as
        // a cube)
        if (im->type == VK_IMAGE_TYPE_3D && type == VK_IMAGE_VIEW_TYPE_2D && g.view_2d_of_3d) base_layer = 0, levels = 1;
        else if (im->type == VK_IMAGE_TYPE_3D && type != VK_IMAGE_VIEW_TYPE_3D) return VK_NULL_HANDLE;
        if (im->type != VK_IMAGE_TYPE_3D && type == VK_IMAGE_VIEW_TYPE_3D) return VK_NULL_HANDLE;
        if ((im->type == VK_IMAGE_TYPE_1D) != (type == VK_IMAGE_VIEW_TYPE_1D || type == VK_IMAGE_VIEW_TYPE_1D_ARRAY))
            return VK_NULL_HANDLE;
        if ((type == VK_IMAGE_VIEW_TYPE_CUBE || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) &&
            (im->layers - base_layer < 6 || im->w != im->h))
            return VK_NULL_HANDLE;
        if (type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && layers < 6) layers = 6;
    }
    VkImageAspectFlags aspect = im->fi.depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                : im->fi.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (for_attachment && im->fi.depth && im->fi.stencil) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    struct {
        uint32_t base_level, levels, base_layer, layers;
        int format, type, sw[4], use;
    } k{base_level, levels, base_layer, layers, (int)format, (int)type, {sw[0], sw[1], sw[2], sw[3]}, use};
    const uint64_t key = hash_bytes(&k, sizeof k);
    auto it = im->views.find(key);
    if (it != im->views.end()) return it->second;
    VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    const VkFormatFeatureFlags ff = features_of(format);
    usage.usage = for_attachment ? (im->fi.depth || im->fi.stencil ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                                   : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
                                 : storage ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT;
    if (storage && (!(ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) || !(im->usage & VK_IMAGE_USAGE_STORAGE_BIT))) {
        im->views[key] = VK_NULL_HANDLE;
        vk_log_limited("storageview", 8, "vk: a storage image of format %d: not storable here", (int)format);
        return VK_NULL_HANDLE;
    }
    if (use == kViewSampled && !(ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return VK_NULL_HANDLE;
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.pNext = &usage;
    ci.image = im->img;
    ci.viewType = type;
    ci.format = format;
    if (use == kViewSampled) ci.components = {swizzle_of(sw[0]), swizzle_of(sw[1]), swizzle_of(sw[2]), swizzle_of(sw[3])};
    ci.subresourceRange = {aspect, base_level, levels, base_layer, layers};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(g.dev, &ci, nullptr, &view) != VK_SUCCESS) view = VK_NULL_HANDLE;
    im->views[key] = view;
    return view;
}

// ---- registrations ----
namespace {
struct TexReg {
    nvn_tex t{};
    nvn_view v{};
    bool has_view = false, ok = false, released = false;
};
std::vector<TexReg> tex_regs;
std::vector<nvn_samp_par> samp_regs;
std::vector<bool> samp_ok;
std::unordered_map<uint64_t, VkSampler> samplers;
std::unordered_map<uint64_t, std::vector<int>> regs_by_serial;
}  // namespace

void register_texture(int id, const nvn_tex* t, const nvn_view* v) {
    if (id < 0 || id >= (1 << 20) || !t) return;
    if ((size_t)id >= tex_regs.size()) tex_regs.resize(id + 1024);
    TexReg& r = tex_regs[id];
    r.t = *t, r.ok = true, r.released = false;
    r.has_view = v != nullptr;
    if (v) r.v = *v;
    regs_by_serial[t->serial].push_back(id);
}

void texture_released(const nvn_tex* t) {
    auto it = regs_by_serial.find(t->serial);
    if (it == regs_by_serial.end()) return;
    for (int id : it->second)
        if ((size_t)id < tex_regs.size() && tex_regs[id].t.serial == t->serial) tex_regs[id].released = true;
    regs_by_serial.erase(it);
}

void register_sampler(int id, const nvn_samp_par* p) {
    if (id < 0 || id >= (1 << 12) || !p) return;
    if ((size_t)id >= samp_regs.size()) samp_regs.resize(id + 256), samp_ok.resize(id + 256);
    samp_regs[id] = *p;
    samp_ok[id] = true;
}

bool texture_of(int id, nvn_tex* t, nvn_view* v, bool* has_view) {
    if (id < 0 || (size_t)id >= tex_regs.size() || !tex_regs[id].ok) return false;
    const TexReg& r = tex_regs[id];
    *t = r.t, *v = r.v, *has_view = r.has_view;
    // (the game re-initialises transient targets at the same storage without registering them again)
    if (r.released) {
        nvn_tex live;
        if (nvn_tex_live_at(r.t.gpu, &live)) *t = live;
    }
    return true;
}

VkSampler sampler_of(int id) {
    nvn_samp_par p{};
    if (id >= 0 && (size_t)id < samp_regs.size() && samp_ok[id]) p = samp_regs[id];
    else p.minf = 1, p.magf = 1, p.lod_max = 1000.0f;
    const uint64_t key = hash_bytes(&p, sizeof p);
    auto it = samplers.find(key);
    if (it != samplers.end()) return it->second;
    auto wrap = [](int m) {
        switch (m) {
        case NVN_WRAP_MODE_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case NVN_WRAP_MODE_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case NVN_WRAP_MODE_MIRROR_CLAMP: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER:
            return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        case NVN_WRAP_MODE_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        }
    };
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = p.magf ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = (p.minf == 1 || p.minf == 3 || p.minf == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = (p.minf == 4 || p.minf == 5) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wrap(p.ws), ci.addressModeV = wrap(p.wt), ci.addressModeW = wrap(p.wr);
    ci.mipLodBias = std::clamp(p.lod_bias, -g.props.limits.maxSamplerLodBias, g.props.limits.maxSamplerLodBias);
    if (p.aniso > 1.0f) ci.anisotropyEnable = VK_TRUE, ci.maxAnisotropy = std::min(p.aniso, g.props.limits.maxSamplerAnisotropy);
    ci.compareEnable = p.cmp_mode != 0;
    ci.compareOp = (VkCompareOp)std::clamp((int)p.cmp_func - 1, 0, 7);
    ci.minLod = p.lod_min;
    ci.maxLod = (p.minf <= 1) ? std::min(p.lod_min, 0.25f) : std::max(p.lod_min, p.lod_max);   // (no mipmapping: the base level)
    VkSamplerCustomBorderColorCreateInfoEXT border{VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    VkSamplerReductionModeCreateInfo red{VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    const void* chain = nullptr;
    if (p.reduction) {
        red.reductionMode = p.reduction == 1 ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX;
        red.pNext = chain, chain = &red;
    }
    const bool border_used = ci.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                             ci.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                             ci.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    if (border_used && g.custom_border) {
        memcpy(border.customBorderColor.float32, p.border, 16);
        border.format = VK_FORMAT_UNDEFINED;
        border.pNext = chain, chain = &border;
        ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
    } else {
        ci.borderColor = p.border[3] > 0.5f ? (p.border[0] > 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK)
                                            : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    ci.pNext = chain;
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(g.dev, &ci, nullptr, &s);
    samplers[key] = s;
    return s;
}

void save_image_bmp(VkImage image, VkFormat format, uint32_t w, uint32_t h, const char* path);
void save_rgba_bmp(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h);
void dump_depth(Image* im, const char* path) {   // (D32F / D16: the depth as grey, near black)
    const uint32_t bpp = im->fi.format == VK_FORMAT_D16_UNORM ? 2 : 4;
    HostBuffer hb = host_buffer((uint64_t)im->w * im->h * bpp, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkCommandBuffer cb = begin_commands();
    full_barrier(cb);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    c.imageExtent = {im->w, im->h, 1};
    vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, hb.buf, 1, &c);
    full_barrier(cb);
    end_and_wait(cb);
    std::vector<uint8_t> rgba((size_t)im->w * im->h * 4);
    float lo = 1, hi = 0;
    for (size_t i = 0; i < (size_t)im->w * im->h; i++) {
        float d;
        if (bpp == 2) d = reinterpret_cast<const uint16_t*>(hb.map)[i] / 65535.0f;
        else memcpy(&d, hb.map + 4 * i, 4);
        lo = std::min(lo, d), hi = std::max(hi, d);
        const uint8_t v = (uint8_t)std::clamp(d * 255.0f, 0.0f, 255.0f);
        rgba[4 * i] = rgba[4 * i + 1] = rgba[4 * i + 2] = v, rgba[4 * i + 3] = 255;
    }
    hos_log("vk: depth %s: %g .. %g", path, lo, hi);
    save_rgba_bmp(path, rgba.data(), im->w, im->h);
    free_host_buffer(hb);
}
void dump_written_images(const char* dir) {
    int n = 0;
    for (const auto& [key, im] : images) {
        if (im->img && im->written && im->fi.depth && !im->fi.stencil && im->samples == VK_SAMPLE_COUNT_1_BIT &&
            im->type == VK_IMAGE_TYPE_2D && im->layers == 1) {
            char path[512];
            snprintf(path, sizeof path, "%s/%03d_%llx_%ux%u_f%x_depth.bmp", dir, n++, (unsigned long long)im->gpu, im->w, im->h, im->d.format);
            dump_depth(im, path);
            continue;
        }
        if (!im->img || !im->written || im->fi.depth || im->fi.stencil || im->fi.integer || im->samples != VK_SAMPLE_COUNT_1_BIT ||
            !(features_of(im->fi.format) & VK_FORMAT_FEATURE_BLIT_SRC_BIT))
            continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%03d_%llx_%ux%u_f%x.bmp", dir, n++, (unsigned long long)im->gpu, im->w, im->h, im->d.format);
        save_image_bmp(im->img, im->fi.format, im->w, im->h, path);
    }
}

/// HOS_VK_TEXDUMP=<gpu hex>,<path> (debugging, with HOS_SCREENSHOT): the texture at that storage address as it is,
/// <path>.now.bmp, and again filled from guest memory, <path>.mem.bmp
void texdump(VkCommandBuffer cb_unused) {
    (void)cb_unused;
    const char* e = getenv("HOS_VK_TEXDUMP");
    if (!e || !strchr(e, ',')) return;
    const uint64_t gpu = strtoull(e, nullptr, 16);
    const std::string path = strchr(e, ',') + 1;
    for (auto& [key, im] : images) {
        if (!im->img || gpu < im->gpu || gpu >= im->gpu + im->L.size) continue;
        hos_log("vk: texdump %#llx %ux%u format %#x target %d written %d", (unsigned long long)im->gpu, im->w, im->h, im->d.format, im->d.target, (int)im->written);
        save_image_bmp(im->img, im->fi.format, im->w, im->h, (path + ".now.bmp").c_str());
        VkCommandBuffer cb = begin_commands();
        fill(cb, im);
        end_and_wait(cb);
        staging_release();
        save_image_bmp(im->img, im->fi.format, im->w, im->h, (path + ".mem.bmp").c_str());
    }
}

void textures_frame_end() {
    frame++;
    hash_budget = kHashBudget;
}

void staging_release() {   // (after a command buffer that used them has finished)
    for (auto& b : staging_used) free_host_buffer(b);
    staging_used.clear();
}


// ---- ReadTexels / WriteTexels: the texture's guest memory, a region at a time ----
namespace {
/// the byte offset of block (bx, by, z) of a level in its layer's block-linear storage (bx in bytes)
struct LevelAddr {
    uint64_t base = 0;
    uint32_t row = 0, rows = 0, gobs_x = 0, bh = 1, bd = 1, blocks_y = 1;
    bool linear = false;
    uint32_t stride = 0;
    uint64_t at(uint32_t xb, uint32_t y, uint32_t z) const {
        if (linear) return base + (uint64_t)y * stride + xb;
        const uint64_t block_row = ((uint64_t)(z / bd) * blocks_y + y / (8 * bh)) * gobs_x;
        const uint64_t in_block = ((uint64_t)(z % bd) * bh + (y / 8) % bh) * 512;
        return base + (block_row + xb / 64) * (512ull * bh * bd) + in_block + gob_offset(xb, y);
    }
};
bool level_addr(const nvn_tex* t, unsigned level, unsigned layer, LevelAddr* a, int* bpb, int* bw, int* bh) {
    *bpb = nvn_fmt_bpb(t->d.format, bw, bh);
    if (*bpb <= 0 || !t->gpu) return false;
    const uint32_t w = mip(std::max(1u, t->d.width), level), h = mip(std::max(1u, t->d.height), level);
    a->row = (w + *bw - 1) / *bw * *bpb, a->rows = (h + *bh - 1) / *bh;
    if (nvn_tex_is_linear(&t->d)) {
        a->linear = true, a->stride = nvn_tex_linear_stride(&t->d), a->base = t->gpu;
        return true;
    }
    unsigned th = 0, td = 0;
    nvn_tex_level_tiles(&t->d, level, &th, &td);
    a->bh = 1u << th, a->bd = 1u << td;
    a->gobs_x = (a->row + 63) / 64;
    a->blocks_y = (a->rows + 8 * a->bh - 1) / (8 * a->bh);
    a->base = t->gpu + (uint64_t)layer * t->L.layer_size + nvn_tex_level_offset(&t->d, level);
    return true;
}
template <bool kWrite>
void texels_guest(const nvn_tex* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, uint8_t* data, size_t row,
                  size_t img) {
    const bool is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    const int depth = std::max(1, r->depth);
    for (int z = 0; z < depth; z++) {
        const unsigned layer = is3d ? layer0 : layer0 + (unsigned)(r->zoffset + z);
        const uint32_t slice = is3d ? (uint32_t)(r->zoffset + z) : 0;
        LevelAddr a;
        int bpb, bw, bh;
        if (!level_addr(t, level, layer, &a, &bpb, &bw, &bh)) return;
        const uint32_t x0 = (uint32_t)std::max(0, r->xoffset) / bw * bpb, y0 = (uint32_t)std::max(0, r->yoffset) / bh;
        const uint32_t wb = (uint32_t)(std::max(0, r->width) + bw - 1) / bw * bpb, hb = (uint32_t)(std::max(0, r->height) + bh - 1) / bh;
        for (uint32_t y = 0; y < hb; y++) {
            uint8_t* line = data + (size_t)z * img + (size_t)y * row;
            for (uint32_t x = 0; x < wb; x += (uint32_t)bpb) {
                const uint64_t at = a.at(x0 + x, y0 + y, slice);
                uint8_t* host = nvn_gpu_host(at);
                if (!host || nvn_gpu_contiguous(at, (uint64_t)bpb) < (uint64_t)bpb) continue;
                if (kWrite) memcpy(host, line + x, (size_t)bpb);
                else memcpy(line + x, host, (size_t)bpb);
            }
        }
    }
}
}  // namespace

void texels_to_guest(const nvn_tex* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, const uint8_t* src,
                     size_t row, size_t img) {
    texels_guest<true>(t, level, layer0, r, const_cast<uint8_t*>(src), row, img);
}
void texels_from_guest(const nvn_tex* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, uint8_t* dst,
                       size_t row, size_t img) {
    texels_guest<false>(t, level, layer0, r, dst, row, img);
}

}  // namespace ovk
