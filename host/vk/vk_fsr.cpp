// (oranges) FSR 1 at the present: the Graphics tab's Scaling "FSR 1". AMD's EASU upscales the picture (the crop of the
// window texture, or the menu's canvas) to the size the window shows it at, then RCAS sharpens it; the present copies
// the result 1:1 into the swapchain image. Only when the picture is shown larger than it is (else the present blits as
// Smooth). Screenshots keep the game's size: they are taken before this.
//
// The passes (host/vk/shaders/fsr1_*.comp on third_party/fidelityfx-fsr1, MIT; compiled into the *_spv.h headers by
// `python -m oranges.shaders`) work on the picture's values as they are shown: gamma-encoded, which is what EASU is
// tuned for. The window texture is RGBA8 (UNORM or sRGB) or wider; either way its stored values are what the window
// shows (the swapchain takes the same encoding, so the plain blit passes them through). So:
//   1. the crop is blitted 1:1 into `src`, RGBA8 of the picture's encoding (UNORM, or sRGB with a UNORM view): the
//      shader reads the stored, gamma-encoded values, whatever the encoding;
//   2. EASU: src (sampled, gathers) -> `mid`, RGBA16F of the shown size;
//   3. RCAS: mid -> `out`, RGBA8 of the picture's encoding again, written through a UNORM view, so the bytes are the
//      gamma-encoded values and the 1:1 blit to the swapchain (same encoding) passes them through unchanged.
// Images stay in GENERAL; each step is separated by a full barrier. Made again when a size or the encoding changes.
#include <cmath>
#include <cstring>

#include "shaders/fsr1_easu_comp_spv.h"
#include "shaders/fsr1_rcas_comp_spv.h"
#include "vk.h"

namespace ovk {
namespace {

struct Pass {
    VkPipeline pipe = VK_NULL_HANDLE;
};
bool tried = false, ready = false;
VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
VkPipelineLayout layout = VK_NULL_HANDLE;
VkSampler sampler = VK_NULL_HANDLE;
Pass easu, rcas;

struct Img {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool fresh = false;   // (made since the last use: its layout is still UNDEFINED)
};
Img src, mid, out;

void free_img(Img& im) {
    if (im.view) vkDestroyImageView(g.dev, im.view, nullptr);
    if (im.image) vkDestroyImage(g.dev, im.image, nullptr);
    if (im.mem) vkFreeMemory(g.dev, im.mem, nullptr);
    im = Img{};
}

/// im as w x h of format, its view in view_format (a UNORM view of an sRGB image: the stored values)
bool make_img(Img& im, uint32_t w, uint32_t h, VkFormat format, VkFormat view_format, VkImageUsageFlags usage) {
    if (im.image && im.w == w && im.h == h && im.format == format) return true;
    if (im.image) {
        vkDeviceWaitIdle(g.dev);
        free_img(im);
    }
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    if (view_format != format)   // (extended usage: storage through the UNORM view of an sRGB image)
        ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {w, h, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    if (vkCreateImage(g.dev, &ii, nullptr, &im.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.dev, im.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g.dev, &ai, nullptr, &im.mem) != VK_SUCCESS) {
        free_img(im);
        return false;
    }
    vkBindImageMemory(g.dev, im.image, im.mem, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = view_format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(g.dev, &vi, nullptr, &im.view) != VK_SUCCESS) {
        free_img(im);
        return false;
    }
    im.w = w, im.h = h, im.format = format, im.fresh = true;
    return true;
}

bool make_pass(Pass& p, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = bytes;
    mi.pCode = code;
    VkShaderModule mod;
    if (vkCreateShaderModule(g.dev, &mi, nullptr, &mod) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = mod;
    ci.stage.pName = "main";
    ci.layout = layout;
    const VkResult r = vkCreateComputePipelines(g.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p.pipe);
    vkDestroyShaderModule(g.dev, mod, nullptr);
    return r == VK_SUCCESS;
}

bool init() {
    if (tried) return ready;
    tried = true;
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0, b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].binding = 1, b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    for (auto& x : b) x.descriptorCount = 1, x.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    si.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    si.bindingCount = 2, si.pBindings = b;
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 80};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1, li.pSetLayouts = &set_layout;
    li.pushConstantRangeCount = 1, li.pPushConstantRanges = &pr;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ready = vkCreateDescriptorSetLayout(g.dev, &si, nullptr, &set_layout) == VK_SUCCESS &&
            vkCreatePipelineLayout(g.dev, &li, nullptr, &layout) == VK_SUCCESS &&
            vkCreateSampler(g.dev, &sci, nullptr, &sampler) == VK_SUCCESS &&
            make_pass(easu, fsr1_easu_comp_spv, sizeof fsr1_easu_comp_spv) &&
            make_pass(rcas, fsr1_rcas_comp_spv, sizeof fsr1_rcas_comp_spv);
    hos_log(ready ? "vk: FSR 1 ready" : "vk: FSR 1 did not start: the present scales as Smooth");
    return ready;
}

uint32_t bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/// ffxFsrPopulateEasuConstants (ffx_fsr1.h), the input viewport the whole input image
void easu_constants(uint32_t con[16], float in_w, float in_h, float out_w, float out_h) {
    con[0] = bits(in_w * (1.0f / out_w));
    con[1] = bits(in_h * (1.0f / out_h));
    con[2] = bits(0.5f * in_w * (1.0f / out_w) - 0.5f);
    con[3] = bits(0.5f * in_h * (1.0f / out_h) - 0.5f);
    con[4] = bits(1.0f / in_w);
    con[5] = bits(1.0f / in_h);
    con[6] = bits(1.0f * (1.0f / in_w));
    con[7] = bits(-1.0f * (1.0f / in_h));
    con[8] = bits(-1.0f * (1.0f / in_w));
    con[9] = bits(2.0f * (1.0f / in_h));
    con[10] = bits(1.0f * (1.0f / in_w));
    con[11] = bits(2.0f * (1.0f / in_h));
    con[12] = bits(0.0f * (1.0f / in_w));
    con[13] = bits(4.0f * (1.0f / in_h));
    con[14] = con[15] = 0;
}

void to_general(VkCommandBuffer cb, Img& im) {
    if (!im.fresh) return;
    im.fresh = false;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}

void dispatch(VkCommandBuffer cb, const Pass& p, VkImageView in, VkImageView store, const void* push, uint32_t size,
              uint32_t w, uint32_t h) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipe);
    VkDescriptorImageInfo ii[2]{};
    ii[0] = {sampler, in, VK_IMAGE_LAYOUT_GENERAL};
    ii[1] = {VK_NULL_HANDLE, store, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w2[2]{};
    for (int i = 0; i < 2; i++) {
        w2[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w2[i].dstBinding = (uint32_t)i;
        w2[i].descriptorCount = 1;
        w2[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w2[i].pImageInfo = &ii[i];
    }
    g.x.vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 2, w2);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, size, push);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
}

}  // namespace

VkImage fsr_upscale(VkCommandBuffer cb, VkImage image, bool srgb, int32_t sx, int32_t sy, uint32_t cw, uint32_t ch,
                    uint32_t dw, uint32_t dh, int sharpness) {
    if (!cw || !ch || !dw || !dh || !init()) return VK_NULL_HANDLE;
    const VkFormat rgba8 = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (!make_img(src, cw, ch, rgba8, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
        !make_img(mid, dw, dh, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
        !make_img(out, dw, dh, rgba8, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        static int logged;
        if (!logged++) hos_log("vk: FSR 1: its images (%ux%u -> %ux%u) could not be made", cw, ch, dw, dh);
        return VK_NULL_HANDLE;
    }
    if (out.fresh) {   // (made again: a new size or encoding)
        static int logged;
        if (logged++ < 8) hos_log("vk: FSR 1: %ux%u -> %ux%u (%s picture)", cw, ch, dw, dh, srgb ? "sRGB" : "UNORM");
    }
    to_general(cb, src), to_general(cb, mid), to_general(cb, out);
    full_barrier(cb);
    VkImageBlit bl{};
    bl.srcSubresource = bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[0] = {sx, sy, 0}, bl.srcOffsets[1] = {sx + (int32_t)cw, sy + (int32_t)ch, 1};
    bl.dstOffsets[1] = {(int32_t)cw, (int32_t)ch, 1};
    vkCmdBlitImage(cb, image, VK_IMAGE_LAYOUT_GENERAL, src.image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
    full_barrier(cb);
    uint32_t con[16];
    easu_constants(con, (float)cw, (float)ch, (float)dw, (float)dh);
    dispatch(cb, easu, src.view, mid.view, con, sizeof con, dw, dh);
    full_barrier(cb);
    // FsrRcasCon: sharpness in stops (0: the most), here 100% -> 0 stops, 10% -> 1.8; Off: copied through
    uint32_t rc[5] = {bits(std::exp2(-(float)(100 - sharpness) / 50.0f)), 0, 0, 0, sharpness > 0 ? 1u : 0u};
    dispatch(cb, rcas, mid.view, out.view, rc, sizeof rc, dw, dh);
    full_barrier(cb);
    return out.image;
}

}  // namespace ovk
