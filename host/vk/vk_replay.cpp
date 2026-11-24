// The rx_nvn_* entry points (host/nvn/rx_nvn.h) on Vulkan: each submit's recordings replayed into one command buffer,
// run to completion before the submit returns (vk.h has the design).
//
// State is kept per queue, as the GPU's channels keep it across command buffers. A draw resolves its program (the bound
// NVN program's stages, translated with the vertex attribute and render target kinds: vk_shader.cpp), pushes its
// descriptors (uniform buffers from guest memory, the driver's banks c0 / c2 built from the bound state, textures
// through the texture pool's registrations), and sets every dynamic state shader objects need. Render passes are
// dynamic rendering, begun at the first draw or clear after the targets change and ended before any transfer.
// What the CPU sees of the GPU's work (events, counters) is written when the submit has finished, in order.
//
// Programs are made ahead of their draws on maker threads (Makers); a render condition waits for the GPU only when the
// work before it may write its counters (render_cond).
//
// Not yet: indirect draws, texel buffers, queries' sample counts, quads (drawn as triangles fans: wrong past one quad),
// alpha test.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/shader_info.h"
#include "vk.h"
#include "vk_shader.h"

extern "C" uint64_t a64_cntpct_host(void);

namespace ovk {

// (vk_texture.cpp)
const FormatInfo& image_format_info(Image* im);
uint32_t image_layers(Image* im);
VkSampleCountFlagBits image_samples(Image* im);
void register_texture(int id, const nvn_tex* t, const nvn_view* v);
void texture_released(const nvn_tex* t);
void register_sampler(int id, const nvn_samp_par* p);
void staging_release();
extern void (*before_transfer)();
Image* image_find(const nvn_tex* t);
void image_dirty(Image* im);
void texels_to_guest(const nvn_tex* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, const uint8_t* src,
                     size_t row, size_t img);
void texels_from_guest(const nvn_tex* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, uint8_t* dst,
                       size_t row, size_t img);
int trace_left();
int images_dirty(uint64_t gpu, uint64_t size);
void dump_depth(Image* im, const char* path);
void save_image_bmp(VkImage image, VkFormat format, uint32_t w, uint32_t h, const char* path);

namespace {

uint64_t hash_bytes(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

constexpr uint32_t kBankSize = 0xA00;     // (a driver bank: c0 / c2)
constexpr uint32_t kUboRange = 0x10000;   // (what a shader's uniform buffer declares: 64 KiB)

// ---- per-submit host memory: driver banks, c1 copies, zeros ----
struct Ring {
    std::vector<HostBuffer> bufs;
    size_t cur = 0;
    uint64_t at = 0;
    BufRef put(const void* data, uint64_t size, uint64_t align = 256) {
        for (;;) {
            if (cur < bufs.size()) {
                HostBuffer& b = bufs[cur];
                const uint64_t a = (at + align - 1) & ~(align - 1);
                if (a + size <= b.size) {
                    if (data) memcpy(b.map + a, data, size);
                    else memset(b.map + a, 0, size);
                    at = a + size;
                    return {b.buf, a, size};
                }
                cur++, at = 0;
                continue;
            }
            bufs.push_back(host_buffer(std::max<uint64_t>(16u << 20, size), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
        }
    }
    void reset() { cur = 0, at = 0; }
} ring;

// ---- state ----
struct StageBind {
    nvn_p_bind_buf ubo[16]{}, ssbo[16]{};
    uint64_t tex[128]{}, samp[128]{}, img[32]{};
};
struct State {
    StageBind st[kNvnStages];
    nvn_prog prog{};
    bool has_prog = false;
    nvn_p_set_rt rt{};
    uint32_t vattr[16]{};
    nvn_vs_st vstream[16]{};
    NVNbufferRange vbuf[16]{};
    nvn_blend_st blend[8]{};
    nvn_color_st color{};
    nvn_chmask_st chmask{0xffffffffu};
    nvn_ms_st ms{};
    nvn_poly_st poly{};
    nvn_ds_st ds{};
    float viewport[4] = {0, 0, 0, 0}, depth_n = 0, depth_f = 1;
    int32_t scissor[4] = {0, 0, 0x7fffffff, 0x7fffffff};
    int stencil_value_mask[2] = {0xff, 0xff}, stencil_mask[2] = {0xff, 0xff}, stencil_ref[2] = {0, 0};
    float poly_offset[3] = {0, 0, 0};
    float blend_color[4] = {0, 0, 0, 0};
    bool depth_clamp = false, raster_discard = false, restart = false, render_enabled = true;
    bool cond_ok = true;   // (SetRenderEnableConditional's condition, when one is set)
    int64_t copy_stride = 0;
    uint32_t grid[3] = {0, 0, 0};   // (the dispatch being pushed: compute's c0 0x200)
    State() {
        for (auto& b : blend) b.src_rgb = NVN_BLEND_FUNC_ONE, b.dst_rgb = NVN_BLEND_FUNC_ZERO, b.src_a = NVN_BLEND_FUNC_ONE,
                              b.dst_a = NVN_BLEND_FUNC_ZERO, b.eq_rgb = NVN_BLEND_EQUATION_ADD, b.eq_a = NVN_BLEND_EQUATION_ADD;
        color.logic_op = NVN_LOGIC_OP_COPY, color.alpha_func = NVN_ALPHA_FUNC_ALWAYS;
        ms.enable = 1;
        poly.cull = NVN_FACE_NONE, poly.front = NVN_FRONT_FACE_CCW, poly.mode = NVN_POLYGON_MODE_FILL;
        ds.dw = 1, ds.df = NVN_DEPTH_FUNC_LESS, ds.sf_f = ds.sf_b = NVN_STENCIL_FUNC_ALWAYS;
        ds.fail_f = ds.zfail_f = ds.zpass_f = ds.fail_b = ds.zfail_b = ds.zpass_b = NVN_STENCIL_OP_KEEP;
    }
};
std::unordered_map<uint64_t, std::unique_ptr<State>> states;   // (by queue)

// ---- the driver banks ----
uint32_t bank_word(const State& s, int stage, uint32_t bank, uint32_t offset) {
    if (stage < 0 || stage >= kNvnStages || (offset & 3)) return 0;
    const StageBind& b = s.st[stage];
    auto lo = [](uint64_t v) { return (uint32_t)v; };
    auto hi = [](uint64_t v) { return (uint32_t)(v >> 32); };
    if (bank == 2) {
        if (offset & 7) return 0;
        if (offset >= 0x568 && offset < 0x568 + 8 * 128) return lo(b.samp[(offset - 0x568) / 8]);
        if (offset >= 0x168 && offset < 0x168 + 8 * 128) return lo(b.tex[(offset - 0x168) / 8]);
        if (offset >= 0x120 && offset < 0x120 + 8 * 9) return lo(b.img[(offset - 0x120) / 8]);
        if (offset >= 0x20 && offset < 0x20 + 8 * 32) return lo(b.tex[(offset - 0x20) / 8]);
        return 0;
    }
    if (bank != 0) return 0;
    const uint32_t base = stage == 0 ? 0x110 : stage == 5 ? 0x310 : stage == 1 ? 0x510 : 0;
    if (base && offset >= base && offset < base + 16 * 16) {
        const nvn_p_bind_buf& e = b.ssbo[(offset - base) / 16];
        const uint32_t in = (offset - base) % 16;
        return in == 0 ? lo(e.addr) : in == 4 ? hi(e.addr) : in == 8 ? (uint32_t)e.size : 0;
    }
    if (stage == 5 && offset >= 0x200 && offset < 0x20c) return s.grid[(offset - 0x200) / 4];
    if (stage == 5 && offset >= 0x210 && offset < 0x210 + 16 * 16) {
        const nvn_p_bind_buf& e = b.ubo[(offset - 0x210) / 16];
        const uint32_t in = (offset - 0x210) % 16;
        return in == 0 ? lo(e.addr) : in == 4 ? hi(e.addr) : in == 8 ? (uint32_t)e.size : 0;
    }
    if (stage == 0 && offset == 0x610) {
        const float point_max = 2047.9375f;
        uint32_t v;
        memcpy(&v, &point_max, 4);
        return v;
    }
    return 0;
}

uint32_t cbuf_word(const State& s, int stage, uint32_t index, uint32_t offset) {
    if (index == 0 || index == 2) return bank_word(s, stage, index, offset);
    if (index >= 3 && index < 3 + 16 && stage >= 0 && stage < kNvnStages) {
        const nvn_p_bind_buf& u = s.st[stage].ubo[index - 3];
        uint32_t v = 0;
        if (u.addr && (uint64_t)offset + 4 <= u.size) nvn_gpu_read(u.addr + offset, &v, 4);
        return v;
    }
    return 0;
}

Shader::TextureType texture_type_of(int target) {
    switch (target) {
    case NVN_TEXTURE_TARGET_1D: return Shader::TextureType::Color1D;
    case NVN_TEXTURE_TARGET_3D: return Shader::TextureType::Color3D;
    case NVN_TEXTURE_TARGET_1D_ARRAY: return Shader::TextureType::ColorArray1D;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: return Shader::TextureType::ColorArray2D;
    case NVN_TEXTURE_TARGET_CUBEMAP: return Shader::TextureType::ColorCube;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: return Shader::TextureType::ColorArrayCube;
    case NVN_TEXTURE_TARGET_BUFFER: return Shader::TextureType::Buffer;
    default: return Shader::TextureType::Color2D;
    }
}

struct Query final : StageQuery {
    const State& s;
    explicit Query(const State& st) : s{st} {}
    uint32_t cbuf_word(int stage, uint32_t index, uint32_t offset) override { return ovk::cbuf_word(s, stage, index, offset); }
    Shader::TextureType texture_type(uint32_t handle) override {
        nvn_tex t;
        nvn_view v;
        bool hv = false;
        if (!texture_of((int)(handle & 0xFFFFF), &t, &v, &hv)) return Shader::TextureType::Color2D;
        return texture_type_of(hv && v.set_target ? v.target : t.d.target);
    }
    bool texture_integer(uint32_t handle) override {
        nvn_tex t;
        nvn_view v;
        bool hv = false;
        if (!texture_of((int)(handle & 0xFFFFF), &t, &v, &hv)) return false;
        return format_info(hv && v.set_format ? v.format : t.d.format).integer;
    }
};

// ---- vertex formats ----
struct VertexFormat {
    VkFormat vk = VK_FORMAT_UNDEFINED;
    Shader::AttributeType type = Shader::AttributeType::Float;
};
VertexFormat vertex_format(int f) {
    using A = Shader::AttributeType;
    switch (f) {
    case 0x01: return {VK_FORMAT_R8_UNORM}; case 0x02: return {VK_FORMAT_R8_SNORM};
    case 0x03: return {VK_FORMAT_R8_UINT, A::UnsignedInt}; case 0x04: return {VK_FORMAT_R8_SINT, A::SignedInt};
    case 0x05: return {VK_FORMAT_R16_SFLOAT}; case 0x06: return {VK_FORMAT_R16_UNORM}; case 0x07: return {VK_FORMAT_R16_SNORM};
    case 0x08: return {VK_FORMAT_R16_UINT, A::UnsignedInt}; case 0x09: return {VK_FORMAT_R16_SINT, A::SignedInt};
    case 0x0A: return {VK_FORMAT_R32_SFLOAT}; case 0x0B: return {VK_FORMAT_R32_UINT, A::UnsignedInt};
    case 0x0C: return {VK_FORMAT_R32_SINT, A::SignedInt};
    case 0x0D: return {VK_FORMAT_R8G8_UNORM}; case 0x0E: return {VK_FORMAT_R8G8_SNORM};
    case 0x0F: return {VK_FORMAT_R8G8_UINT, A::UnsignedInt}; case 0x10: return {VK_FORMAT_R8G8_SINT, A::SignedInt};
    case 0x11: return {VK_FORMAT_R16G16_SFLOAT}; case 0x12: return {VK_FORMAT_R16G16_UNORM};
    case 0x13: return {VK_FORMAT_R16G16_SNORM}; case 0x14: return {VK_FORMAT_R16G16_UINT, A::UnsignedInt};
    case 0x15: return {VK_FORMAT_R16G16_SINT, A::SignedInt}; case 0x16: return {VK_FORMAT_R32G32_SFLOAT};
    case 0x17: return {VK_FORMAT_R32G32_UINT, A::UnsignedInt}; case 0x18: return {VK_FORMAT_R32G32_SINT, A::SignedInt};
    case 0x19: return {VK_FORMAT_R8G8B8_UNORM}; case 0x1A: return {VK_FORMAT_R8G8B8_SNORM};
    case 0x1B: return {VK_FORMAT_R8G8B8_UINT, A::UnsignedInt}; case 0x1C: return {VK_FORMAT_R8G8B8_SINT, A::SignedInt};
    case 0x1D: return {VK_FORMAT_R16G16B16_SFLOAT}; case 0x1E: return {VK_FORMAT_R16G16B16_UNORM};
    case 0x1F: return {VK_FORMAT_R16G16B16_SNORM}; case 0x20: return {VK_FORMAT_R16G16B16_UINT, A::UnsignedInt};
    case 0x21: return {VK_FORMAT_R16G16B16_SINT, A::SignedInt}; case 0x22: return {VK_FORMAT_R32G32B32_SFLOAT};
    case 0x23: return {VK_FORMAT_R32G32B32_UINT, A::UnsignedInt}; case 0x24: return {VK_FORMAT_R32G32B32_SINT, A::SignedInt};
    case 0x25: case 0x6D: return {VK_FORMAT_R8G8B8A8_UNORM}; case 0x26: case 0x6E: return {VK_FORMAT_R8G8B8A8_SNORM};
    case 0x27: case 0x6F: return {VK_FORMAT_R8G8B8A8_UINT, A::UnsignedInt};
    case 0x28: case 0x70: return {VK_FORMAT_R8G8B8A8_SINT, A::SignedInt};
    case 0x29: case 0x71: return {VK_FORMAT_R16G16B16A16_SFLOAT}; case 0x2A: case 0x72: return {VK_FORMAT_R16G16B16A16_UNORM};
    case 0x2B: case 0x73: return {VK_FORMAT_R16G16B16A16_SNORM};
    case 0x2C: case 0x74: return {VK_FORMAT_R16G16B16A16_UINT, A::UnsignedInt};
    case 0x2D: case 0x75: return {VK_FORMAT_R16G16B16A16_SINT, A::SignedInt};
    case 0x2E: case 0x76: return {VK_FORMAT_R32G32B32A32_SFLOAT};
    case 0x2F: case 0x77: return {VK_FORMAT_R32G32B32A32_UINT, A::UnsignedInt};
    case 0x30: case 0x78: return {VK_FORMAT_R32G32B32A32_SINT, A::SignedInt};
    case 0x3D: return {VK_FORMAT_A2B10G10R10_UNORM_PACK32}; case 0x3E: return {VK_FORMAT_A2B10G10R10_UINT_PACK32, A::UnsignedInt};
    case 0x3F: return {VK_FORMAT_B10G11R11_UFLOAT_PACK32};
    case 0x51: return {VK_FORMAT_R8_UINT, A::UnsignedScaled}; case 0x52: return {VK_FORMAT_R8_SINT, A::SignedScaled};
    case 0x53: return {VK_FORMAT_R16_UINT, A::UnsignedScaled}; case 0x54: return {VK_FORMAT_R16_SINT, A::SignedScaled};
    case 0x55: return {VK_FORMAT_R32_UINT, A::UnsignedScaled}; case 0x56: return {VK_FORMAT_R32_SINT, A::SignedScaled};
    case 0x57: return {VK_FORMAT_R8G8_UINT, A::UnsignedScaled}; case 0x58: return {VK_FORMAT_R8G8_SINT, A::SignedScaled};
    case 0x59: return {VK_FORMAT_R16G16_UINT, A::UnsignedScaled}; case 0x5A: return {VK_FORMAT_R16G16_SINT, A::SignedScaled};
    case 0x5B: return {VK_FORMAT_R32G32_UINT, A::UnsignedScaled}; case 0x5C: return {VK_FORMAT_R32G32_SINT, A::SignedScaled};
    case 0x5D: return {VK_FORMAT_R8G8B8_UINT, A::UnsignedScaled}; case 0x5E: return {VK_FORMAT_R8G8B8_SINT, A::SignedScaled};
    case 0x5F: return {VK_FORMAT_R16G16B16_UINT, A::UnsignedScaled}; case 0x60: return {VK_FORMAT_R16G16B16_SINT, A::SignedScaled};
    case 0x61: return {VK_FORMAT_R32G32B32_UINT, A::UnsignedScaled}; case 0x62: return {VK_FORMAT_R32G32B32_SINT, A::SignedScaled};
    case 0x63: return {VK_FORMAT_R8G8B8A8_UINT, A::UnsignedScaled}; case 0x64: return {VK_FORMAT_R8G8B8A8_SINT, A::SignedScaled};
    case 0x65: return {VK_FORMAT_R16G16B16A16_UINT, A::UnsignedScaled};
    case 0x66: return {VK_FORMAT_R16G16B16A16_SINT, A::SignedScaled};
    case 0x67: return {VK_FORMAT_R32G32B32A32_UINT, A::UnsignedScaled};
    case 0x68: return {VK_FORMAT_R32G32B32A32_SINT, A::SignedScaled};
    case 0x69: return {VK_FORMAT_A2B10G10R10_SNORM_PACK32}; case 0x6A: return {VK_FORMAT_A2B10G10R10_SINT_PACK32, A::SignedInt};
    case 0x6B: return {VK_FORMAT_A2B10G10R10_UINT_PACK32, A::UnsignedScaled};
    case 0x6C: return {VK_FORMAT_A2B10G10R10_SINT_PACK32, A::SignedScaled};
    case 0x99: case 0x9A: return {VK_FORMAT_B8G8R8A8_UNORM};
    default: return {};
    }
}

// ---- programs ----
enum BindKind { kCbuf, kSsbo, kTexBuf, kImgBuf, kTex, kImg };
struct Bind {
    BindKind kind;
    int stage;            // (NVN stage)
    uint32_t binding, count;
    uint32_t index = 0, offset = 0, shift = 0, sec_index = 0, sec_offset = 0, sec_shift = 0, size_shift = 3;
    bool has_secondary = false;
    int tex_type = 0;     // (Shader::TextureType)
    bool written = false; // (a storage buffer the program writes)
};
struct Program {
    // (made by the submitting thread, or ahead of its draw by a maker thread: claimed by whichever starts it; ready once
    // made, ok or not)
    std::atomic<bool> claimed{false}, ready{false};
    bool ok = false;
    std::string why;
    std::vector<std::shared_ptr<const StageCode>> codes;
    uint64_t key = 0;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<VkShaderEXT> shaders;
    std::vector<VkShaderStageFlagBits> stages;
    std::vector<Bind> binds;
    std::vector<VkDescriptorSetLayoutBinding> layout_bindings;
    std::vector<bool> loads_generic;   // (the vertex stage's inputs)
};
std::unordered_map<uint64_t, std::unique_ptr<Program>> programs;
constexpr uint32_t kPushBytes = 128;

VkShaderStageFlagBits vk_stage(Shader::Stage s) {
    switch (s) {
    case Shader::Stage::VertexB: return VK_SHADER_STAGE_VERTEX_BIT;
    case Shader::Stage::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case Shader::Stage::Geometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
    case Shader::Stage::TessellationControl: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    case Shader::Stage::TessellationEval: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    default: return VK_SHADER_STAGE_COMPUTE_BIT;
    }
}

void make_binds(Program& p, const Translated& t) {
    uint32_t binding = 0;
    auto add = [&](VkDescriptorType type, uint32_t count, VkShaderStageFlags stages) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = binding, b.descriptorType = type, b.descriptorCount = std::max(1u, count), b.stageFlags = stages;
        p.layout_bindings.push_back(b);
    };
    for (const auto& s : t.stages) {
        const VkShaderStageFlags vs = vk_stage(s.stage);
        const Shader::Info& in = s.info;
        // (as spirv_emit_context.cpp numbers them: uniform buffers, storage buffers, texel buffers, image buffers,
        // textures, images)
        const uint32_t cbuf_base = binding;
        uint32_t k = 0;
        for (const auto& d : in.constant_buffer_descriptors) {
            binding = cbuf_base + k;
            Bind b{kCbuf, s.nvn_stage, binding, d.count};
            b.index = d.index;
            p.binds.push_back(b);
            add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, d.count, vs);
            k += d.count;
        }
        binding = cbuf_base + (uint32_t)in.constant_buffer_descriptors.size();
        for (const auto& d : in.storage_buffers_descriptors) {
            if (!d.count) continue;
            Bind b{kSsbo, s.nvn_stage, binding, d.count};
            b.index = d.cbuf_index, b.offset = d.cbuf_offset, b.written = d.is_written;
            p.binds.push_back(b);
            add(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, d.count, vs);
            binding += 1;
        }
        for (const auto& d : in.texture_buffer_descriptors) {
            Bind b{kTexBuf, s.nvn_stage, binding, d.count};
            b.index = d.cbuf_index, b.offset = d.cbuf_offset;
            p.binds.push_back(b);
            add(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, d.count, vs);
            binding++;
        }
        for (const auto& d : in.image_buffer_descriptors) {
            Bind b{kImgBuf, s.nvn_stage, binding, d.count};
            b.index = d.cbuf_index, b.offset = d.cbuf_offset;
            p.binds.push_back(b);
            add(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, d.count, vs);
            binding++;
        }
        for (const auto& d : in.texture_descriptors) {
            Bind b{kTex, s.nvn_stage, binding, d.count};
            b.index = d.cbuf_index, b.offset = d.cbuf_offset, b.shift = d.shift_left;
            b.has_secondary = d.has_secondary, b.sec_index = d.secondary_cbuf_index, b.sec_offset = d.secondary_cbuf_offset;
            b.sec_shift = d.secondary_shift_left, b.size_shift = d.size_shift, b.tex_type = (int)d.type;
            p.binds.push_back(b);
            add(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, d.count, vs);
            binding++;
        }
        for (const auto& d : in.image_descriptors) {
            Bind b{kImg, s.nvn_stage, binding, d.count};
            b.index = d.cbuf_index, b.offset = d.cbuf_offset, b.size_shift = d.size_shift, b.tex_type = (int)d.type;
            p.binds.push_back(b);
            add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, d.count, vs);
            binding++;
        }
    }
}

/// (a check) the descriptor bindings a SPIR-V module declares: binding -> (descriptor type, count)
std::unordered_map<uint32_t, std::pair<int, uint32_t>> spirv_bindings(const std::vector<uint32_t>& w) {
    std::unordered_map<uint32_t, uint32_t> binding_of, ptr_class, ptr_type, var_ptr, array_len, array_elem, constant;
    std::unordered_map<uint32_t, int> type_kind;   // 1 sampled image, 2 image (storage), 3 image buffer sampled, 4 storage texel, 5 struct
    std::vector<uint32_t> vars;
    for (size_t i = 5; i < w.size();) {
        const uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
        if (!n) break;
        const uint32_t* a = &w[i + 1];
        if (op == 71 && n >= 4 && a[1] == 33) binding_of[a[0]] = a[2];
        else if (op == 32) ptr_class[a[0]] = a[1], ptr_type[a[0]] = a[2];
        else if (op == 59) var_ptr[a[1]] = a[0], vars.push_back(a[1]);
        else if (op == 27) type_kind[a[0]] = 1;
        else if (op == 25) type_kind[a[0]] = a[2] == 5 ? (a[6] == 2 ? 4 : 3) : (a[6] == 2 ? 2 : 6);
        else if (op == 30) type_kind[a[0]] = 5;
        else if (op == 28) array_elem[a[0]] = a[1], array_len[a[0]] = a[2];
        else if (op == 43 && n >= 4) constant[a[1]] = a[2];
        i += n;
    }
    std::unordered_map<uint32_t, std::pair<int, uint32_t>> out;
    for (uint32_t v : vars) {
        auto b = binding_of.find(v);
        if (b == binding_of.end()) continue;
        const uint32_t ptr = var_ptr[v], cls = ptr_class[ptr];
        uint32_t t = ptr_type[ptr], count = 1;
        if (array_elem.count(t) && type_kind[array_elem[t]] != 5) count = constant[array_len[t]], t = array_elem[t];
        int vk = -1;
        if (cls == 2) vk = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else if (cls == 12) vk = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (cls == 0) {
            const int k = type_kind[t];
            vk = k == 1 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : k == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                 : k == 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : k == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : -2;
        }
        out[b->second] = {vk, count};
    }
    return out;
}

/// the inputs' fields (not the struct's bytes: its padding is not set)
uint64_t inputs_hash(const PipelineInputs& in) {
    uint64_t h = hash_bytes(in.attributes.data(), sizeof in.attributes);
    h = hash_bytes(in.color_outputs.data(), sizeof in.color_outputs, h);
    return hash_bytes(&in.convert_depth, sizeof in.convert_depth, h);
}

/// the bound program's stages (graphics or compute) and its key, for these inputs; 0 when it has none to run
uint64_t program_key(const State& s, const PipelineInputs& in, bool compute, std::vector<std::shared_ptr<const StageCode>>& codes) {
    codes.clear();
    uint64_t key = inputs_hash(in);
    for (uint32_t i = 0; i < s.prog.nstage && i < 6; i++) {
        auto c = stage_code(s.prog.sd[i]);
        if (!c || !c->ok) return 0;
        if ((c->nvn_stage == 5) != compute) continue;
        codes.push_back(c);
    }
    if (codes.empty()) return 0;
    std::sort(codes.begin(), codes.end(), [](const auto& a, const auto& b) { return (int)a->stage < (int)b->stage; });
    for (const auto& c : codes) key = key * 31 + c->key;
    return key ? key : 1;
}

/// translates the program's stages (p.codes) with the state of its draw and makes its shaders: on the submitting thread
/// or a maker thread
void make_program(Program& p, const State& s, const PipelineInputs& in) {
    const std::vector<std::shared_ptr<const StageCode>>& codes = p.codes;
    Query q{s};
    const int64_t t0 = hos_now_ns();
    Translated t;
    {
        StatTimer st{kStatTranslate};
        t = translate(codes, in, q);
    }
    if (!t.ok) {
        p.why = t.why;
        vk_log_limited("translate", 32, "vk: a program does not translate: %s", t.why.c_str());
        return;
    }
    make_binds(p, t);
    if (const char* dir = getenv("HOS_VK_SPV")) {   // (debugging: each program's SPIR-V, <dir>/<key>_<stage>.spv)
        for (const auto& st : t.stages) {
            char path[512];
            snprintf(path, sizeof path, "%s/%016llx_%d.spv", dir, (unsigned long long)p.key, st.nvn_stage);
            if (FILE* f = fopen(path, "wb")) fwrite(st.spirv.data(), 4, st.spirv.size(), f), fclose(f);
        }
    }
    {   // (the layout against what the modules declare: a mismatch is a numbering bug here)
        for (const auto& st : t.stages)
            for (const auto& [binding, d] : spirv_bindings(st.spirv)) {
                const VkDescriptorSetLayoutBinding* lb = nullptr;
                for (const auto& l : p.layout_bindings) if (l.binding == binding) lb = &l;
                if (!lb || (int)lb->descriptorType != d.first || lb->descriptorCount != d.second)
                    vk_log_limited("bindcheck", 16, "vk: binding %u: the module declares type %d x%u, the layout has %d x%u",
                                   binding, d.first, d.second, lb ? (int)lb->descriptorType : -1, lb ? lb->descriptorCount : 0);
            }
    }
    VkDescriptorSetLayoutCreateInfo dci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dci.bindingCount = (uint32_t)p.layout_bindings.size();
    dci.pBindings = p.layout_bindings.data();
    uint32_t descriptors = 0;
    for (const auto& b : p.layout_bindings) descriptors += b.descriptorCount;
    VkPushConstantRange pc{VK_SHADER_STAGE_ALL, 0, kPushBytes};
    if (descriptors > 32 || vkCreateDescriptorSetLayout(g.dev, &dci, nullptr, &p.dsl) != VK_SUCCESS) {
        p.why = "too many descriptors to push (" + std::to_string(descriptors) + ")";
        vk_log_limited("translate", 32, "vk: a program has %u descriptors: more than can be pushed", descriptors);
        return;
    }
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1, pli.pSetLayouts = &p.dsl;
    pli.pushConstantRangeCount = 1, pli.pPushConstantRanges = &pc;
    vkCreatePipelineLayout(g.dev, &pli, nullptr, &p.layout);
    std::vector<VkShaderCreateInfoEXT> cis;
    for (size_t i = 0; i < t.stages.size(); i++) {
        VkShaderCreateInfoEXT ci{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
        ci.flags = t.stages.size() > 1 ? VK_SHADER_CREATE_LINK_STAGE_BIT_EXT : 0;
        ci.stage = vk_stage(t.stages[i].stage);
        ci.nextStage = i + 1 < t.stages.size() ? vk_stage(t.stages[i + 1].stage) : 0;
        if (ci.stage == VK_SHADER_STAGE_VERTEX_BIT && !ci.nextStage) ci.nextStage = VK_SHADER_STAGE_FRAGMENT_BIT;
        ci.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
        ci.codeSize = t.stages[i].spirv.size() * 4;
        ci.pCode = t.stages[i].spirv.data();
        ci.pName = "main";
        ci.setLayoutCount = 1, ci.pSetLayouts = &p.dsl;
        ci.pushConstantRangeCount = 1, ci.pPushConstantRanges = &pc;
        cis.push_back(ci);
        p.stages.push_back(ci.stage);
    }
    p.shaders.resize(cis.size());
    VkResult r;
    {
        StatTimer st{kStatShaders};
        r = g.x.vkCreateShadersEXT(g.dev, (uint32_t)cis.size(), cis.data(), nullptr, p.shaders.data());
    }
    if (r != VK_SUCCESS) {
        p.why = "vkCreateShadersEXT " + std::to_string((int)r);
        vk_log_limited("translate", 32, "vk: shaders not made: %d", (int)r);
        return;
    }
    for (const auto& st : t.stages)
        if (st.stage == Shader::Stage::VertexB) {
            p.loads_generic.resize(32);
            for (int i = 0; i < 32; i++) p.loads_generic[i] = st.info.loads.Generic(i);
        }
    p.ok = true;
    static std::atomic<int> n;
    static std::atomic<int64_t> total;   // (translation and shader creation: on maker threads, or the submitting thread's)
    total += hos_now_ns() - t0;
    if (const int k = ++n; k % 100 == 1)
        hos_log("vk: %d programs made (the last in %.1f ms, %.0f ms in all)", k, (hos_now_ns() - t0) / 1e6, total / 1e6);
}

/// Programs made ahead: at each submit, before its replay, the recordings are read for the programs their draws and
/// dispatches will need (the state followed as the replay follows it), and those not made yet are made by maker threads,
/// with a copy of the state of the draw that needs them, several at once. The replay takes each when its draw comes,
/// waiting for it if a maker is still at it, or makes it itself if none has started it. A program costs ~0.5 ms of
/// translation and ~2 ms of the driver's shader creation; a new area meets dozens at once (~90 by the title screen).
struct Job {
    Program* p;
    std::shared_ptr<const State> s;
    PipelineInputs in;
};
struct Makers {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Job> jobs;
    int threads = -1;   // (-1: not started; 0: none, HOS_VK_MAKERS=0)
    void run() {
        for (;;) {
            Job j;
            {
                std::unique_lock lk(mu);
                cv.wait(lk, [&] { return !jobs.empty(); });
                j = std::move(jobs.front());
                jobs.pop_front();
            }
            if (j.p->claimed.exchange(true)) continue;   // (the replay came to it first)
            make_program(*j.p, *j.s, j.in);
            j.p->ready.store(true, std::memory_order_release);
            j.p->ready.notify_all();
        }
    }
    void start() {
        const char* e = getenv("HOS_VK_MAKERS");
        const int cores = (int)std::thread::hardware_concurrency();
        threads = e ? std::clamp(atoi(e), 0, 32) : std::clamp(cores / 3, 1, 6);
        for (int i = 0; i < threads; i++) std::thread([this] { run(); }).detach();
        hos_log("vk: %d threads make programs ahead of their draws", threads);
    }
    void push(Job j) {
        {
            std::lock_guard lk(mu);
            jobs.push_back(std::move(j));
        }
        cv.notify_one();
    }
} makers;

/// the program for a draw or dispatch with state s (null when there is none to run)
Program* program_for(const State& s, const PipelineInputs& in, bool compute) {
    StatTimer timer{kStatProgram};
    std::vector<std::shared_ptr<const StageCode>> codes;
    const uint64_t key = program_key(s, in, compute, codes);
    if (!key) return nullptr;
    auto it = programs.find(key);
    if (it == programs.end()) {
        auto p = std::make_unique<Program>();
        p->codes = std::move(codes);
        p->key = key;
        it = programs.emplace(key, std::move(p)).first;
    }
    Program& p = *it->second;
    if (!p.ready.load(std::memory_order_acquire)) {
        if (!p.claimed.exchange(true)) {
            make_program(p, s, in);
            p.ready.store(true, std::memory_order_release);
        } else {
            StatTimer wait{kStatProgramWait};
            p.ready.wait(false, std::memory_order_acquire);
        }
    }
    return p.ok ? &p : nullptr;
}

/// a program a draw or dispatch ahead will need, made by a maker thread if it is new
void program_ahead(const State& s, const PipelineInputs& in, bool compute) {
    std::vector<std::shared_ptr<const StageCode>> codes;
    const uint64_t key = program_key(s, in, compute, codes);
    if (!key || programs.count(key)) return;
    auto p = std::make_unique<Program>();
    p->codes = std::move(codes);
    p->key = key;
    makers.push({p.get(), std::make_shared<const State>(s), in});
    programs.emplace(key, std::move(p));
}

// ---- the replay ----
struct CpuOp {   // (what the CPU sees, done after the GPU's work)
    int op;
    uint8_t bytes[sizeof(nvn_p_signal) > sizeof(nvn_p_counter) ? sizeof(nvn_p_signal) : sizeof(nvn_p_counter)];
};

struct Replay {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    State* s = nullptr;
    bool rendering = false;
    uint32_t rw = 0, rh = 0;                  // (the render area)
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    int ncolor = 0;
    VkFormat color_formats[8]{};
    bool color_integer[8]{}, color_sint[8]{};
    bool has_depth = false, has_stencil = false;
    std::vector<CpuOp> cpu;
    // (guest memory the recorded GPU work not run yet writes, [begin, end); unknown: somewhere not tracked)
    std::vector<std::pair<uint64_t, uint64_t>> writes;
    bool writes_unknown = false;
    bool scanning = false;   // (run() follows the state only, for the programs ahead: rx_nvn_submit_q)
    std::vector<Image*> stored;   // (the storage images of the draw or dispatch being pushed: written by the GPU)
    uint64_t draws = 0, skipped = 0, dispatches = 0;
} rp;

/// the recorded GPU work writes guest memory [gpu, gpu + size)
void gpu_writes(uint64_t gpu, uint64_t size) {
    if (!rp.writes.empty() && rp.writes.back().second == gpu) rp.writes.back().second = gpu + size;
    else if (rp.writes.size() < 4096) rp.writes.push_back({gpu, gpu + size});
    else rp.writes_unknown = true;
}
/// whether the GPU work recorded and not run yet may write guest memory [gpu, gpu + size) (its CPU side aside)
bool gpu_may_write(uint64_t gpu, uint64_t size) {
    if (rp.writes_unknown) return true;
    const uint64_t end = gpu + size;
    for (const auto& [a, b] : rp.writes)
        if (a < end && gpu < b) return true;
    return false;
}

void end_rendering() {
    if (!rp.rendering) return;
    vkCmdEndRendering(rp.cb);
    full_barrier(rp.cb);
    rp.rendering = false;
}
void before_transfer_hook() { end_rendering(); }

/// begins rendering to the bound targets; false when there is nothing to render to
bool begin_rendering() {
    if (rp.rendering) return true;
    const State& s = *rp.s;
    VkRenderingAttachmentInfo colors[8]{};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    uint32_t w = ~0u, h = ~0u;
    rp.ncolor = 0, rp.has_depth = rp.has_stencil = false;
    rp.samples = VK_SAMPLE_COUNT_1_BIT;
    bool any = false;
    const int n = std::clamp(s.rt.n, 0, 8);
    for (int i = 0; i < n; i++) {
        colors[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colors[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        rp.color_formats[i] = VK_FORMAT_UNDEFINED, rp.color_integer[i] = rp.color_sint[i] = false;
        Image* im = image_of(rp.cb, s.rt.color[i]);
        if (!im) continue;
        const VkImageView v = view_of(im, &s.rt.cview[i], s.rt.has_cview[i], 0, kViewAttachment);
        if (!v) continue;
        image_written(im);
        colors[i].imageView = v;
        const unsigned level = s.rt.has_cview[i] && s.rt.cview[i].set_levels ? s.rt.cview[i].base_level : 0;
        uint32_t iw, ih;
        image_extent(im, level, &iw, &ih);
        w = std::min(w, iw), h = std::min(h, ih);
        rp.color_formats[i] = image_format(im);
        rp.color_integer[i] = image_format_info(im).integer, rp.color_sint[i] = image_format_info(im).sint;
        rp.samples = image_samples(im);
        any = true;
    }
    rp.ncolor = n;
    if (s.rt.depth) {
        Image* im = image_of(rp.cb, s.rt.depth);
        if (im) {
            const VkImageView v = view_of(im, &s.rt.dview, s.rt.has_dview, 0, kViewAttachment);
            if (v) {
                image_written(im);
                depth.imageView = v;
                depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                uint32_t iw, ih;
                const unsigned level = s.rt.has_dview && s.rt.dview.set_levels ? s.rt.dview.base_level : 0;
                image_extent(im, level, &iw, &ih);
                w = std::min(w, iw), h = std::min(h, ih);
                rp.has_depth = image_format_info(im).depth, rp.has_stencil = image_format_info(im).stencil;
                if (!any) rp.samples = image_samples(im);
                any = true;
            }
        }
    }
    if (!any) return false;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = (uint32_t)n;
    ri.pColorAttachments = colors;
    if (depth.imageView) {
        if (rp.has_depth) ri.pDepthAttachment = &depth;
        if (rp.has_stencil) ri.pStencilAttachment = &depth;
    }
    vkCmdBeginRendering(rp.cb, &ri);
    rp.rendering = true, rp.rw = w, rp.rh = h;
    return true;
}

VkRect2D scissor_rect() {
    const State& s = *rp.s;
    int64_t x = std::max(0, s.scissor[0]), y = std::max(0, s.scissor[1]);
    int64_t x2 = std::min<int64_t>((int64_t)s.scissor[0] + s.scissor[2], rp.rw);
    int64_t y2 = std::min<int64_t>((int64_t)s.scissor[1] + s.scissor[3], rp.rh);
    x = std::min<int64_t>(x, rp.rw), y = std::min<int64_t>(y, rp.rh);
    return {{(int32_t)x, (int32_t)y}, {(uint32_t)std::max<int64_t>(0, x2 - x), (uint32_t)std::max<int64_t>(0, y2 - y)}};
}

void clear_color(const nvn_p_clear_color& c) {
    if (!rp.s->render_enabled || !rp.s->cond_ok) return;
    if (trace_left()) hos_log("vk trace: clear %d to %g %g %g %g mask %#x (scissor %d %d %d %d, render %d)", c.index, c.rgba[0], c.rgba[1],
                              c.rgba[2], c.rgba[3], c.mask, rp.s->scissor[0], rp.s->scissor[1], rp.s->scissor[2], rp.s->scissor[3],
                              (int)rp.s->render_enabled);
    if (!begin_rendering() || c.index < 0 || c.index >= rp.ncolor || !rp.color_formats[c.index]) return;
    VkClearAttachment a{VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)c.index, {}};
    if (rp.color_integer[c.index]) {
        for (int i = 0; i < 4; i++) {
            if (rp.color_sint[c.index]) a.clearValue.color.int32[i] = (int32_t)c.rgba[i];
            else a.clearValue.color.uint32[i] = (uint32_t)c.rgba[i];
        }
    } else {
        memcpy(a.clearValue.color.float32, c.rgba, 16);
    }
    VkClearRect r{scissor_rect(), 0, 1};
    if (!r.rect.extent.width || !r.rect.extent.height) return;
    // (a channel mask: not all channels cleared is not done by a clear)
    if ((c.mask & 0xf) != 0xf && c.mask) vk_log_limited("clearmask", 4, "vk: ClearColor with channel mask %#x: all cleared", c.mask);
    if (!c.mask) return;
    vkCmdClearAttachments(rp.cb, 1, &a, 1, &r);
}

void clear_ds(const nvn_p_clear_ds& c) {
    if (!rp.s->render_enabled || !rp.s->cond_ok) return;
    if (trace_left()) hos_log("vk trace: clear depth %g (mask %d) stencil %d (mask %#x)", c.depth, c.depth_mask, c.stencil, c.stencil_mask);
    if (!begin_rendering() || (!rp.has_depth && !rp.has_stencil)) return;
    VkClearAttachment a{};
    if (rp.has_depth && c.depth_mask) a.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (rp.has_stencil && c.stencil_mask) a.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!a.aspectMask) return;
    a.clearValue.depthStencil = {std::clamp(c.depth, 0.0f, 1.0f), (uint32_t)c.stencil};
    VkClearRect r{scissor_rect(), 0, 1};
    if (!r.rect.extent.width || !r.rect.extent.height) return;
    vkCmdClearAttachments(rp.cb, 1, &a, 1, &r);
}

VkBlendFactor blend_factor(int f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
VkBlendOp blend_op(int e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}
VkStencilOp stencil_op(int o) {
    switch (o) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}
VkCompareOp compare_op(int f) { return (VkCompareOp)std::clamp(f - 1, 0, 7); }
VkPrimitiveTopology topology(int prim, bool* ok) {
    *ok = true;
    switch (prim) {
    case NVN_DRAW_PRIMITIVE_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case NVN_DRAW_PRIMITIVE_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLES: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: case NVN_DRAW_PRIMITIVE_POLYGON: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_PATCHES: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    case NVN_DRAW_PRIMITIVE_QUADS: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // (converted: quad_indices)
    default: *ok = false; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

/// the handle a texture or image descriptor reads, element k
uint32_t descriptor_handle(const State& s, const Bind& b, uint32_t k) {
    const uint32_t off = b.offset + (k << b.size_shift);
    uint32_t h = cbuf_word(s, b.stage, b.index, off) << b.shift;
    if (b.has_secondary) h |= cbuf_word(s, b.stage, b.sec_index, b.sec_offset) << b.sec_shift;
    return h;
}

VkSampler default_sampler() {
    static VkSampler s = sampler_of(-1);
    return s;
}

/// pushes the program's descriptors; false when the draw cannot be made
bool trace_on = false;   // (the draw being pushed is traced)
bool push_descriptors(const Program& p, VkPipelineBindPoint point) {
    StatTimer timer{kStatDescriptors};
    const State& s = *rp.s;
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> bufs;
    std::vector<VkDescriptorImageInfo> imgs;
    bufs.reserve(64), imgs.reserve(64);
    std::vector<std::pair<size_t, size_t>> where;   // (each write's first info, buffer or image)
    for (const Bind& b : p.binds) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = b.binding;
        w.descriptorCount = std::max(1u, b.count);
        if (b.kind == kCbuf) {
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            BufRef r;
            if (b.index == 0 || b.index == 2) {
                uint8_t bank[kBankSize];
                for (uint32_t o = 0; o < kBankSize; o += 4) {
                    const uint32_t v = bank_word(s, b.stage, b.index, o);
                    memcpy(bank + o, &v, 4);
                }
                r = ring.put(bank, kBankSize);
            } else if (b.index == 1) {
                const StageCode* c = nullptr;
                for (const auto& code : p.codes) if (code->nvn_stage == b.stage) c = code.get();
                r = c && !c->c1.empty() ? ring.put(c->c1.data(), c->c1.size()) : ring.put(nullptr, 256);
            } else if (b.index - 3 < 16) {
                const nvn_p_bind_buf& u = s.st[b.stage].ubo[b.index - 3];
                if (u.addr && u.size) r = buffer_at(u.addr, std::min<uint64_t>(u.size, 16));
                if (r.buf && (r.offset & (g.props.limits.minUniformBufferOffsetAlignment - 1))) r = {};   // (copied)
                if (r.buf) r.size = std::min<uint64_t>({r.size, u.size, kUboRange});
                else if (u.addr && u.size) {   // (not in an imported pool, or misaligned: a copy)
                    std::vector<uint8_t> tmp(std::min<uint64_t>(u.size, kUboRange));
                    nvn_gpu_read(u.addr, tmp.data(), tmp.size());
                    r = ring.put(tmp.data(), tmp.size());
                }
            }
            bufs.push_back({r.buf, r.offset, r.buf ? std::max<uint64_t>(r.size, 4) : VK_WHOLE_SIZE});
            for (uint32_t k = 1; k < w.descriptorCount; k++) bufs.push_back(bufs.back());
            where.push_back({bufs.size() - w.descriptorCount, 0});
        } else if (b.kind == kSsbo) {
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            const uint64_t addr = cbuf_word(s, b.stage, b.index, b.offset) |
                                  (uint64_t)cbuf_word(s, b.stage, b.index, b.offset + 4) << 32;
            const uint32_t size = cbuf_word(s, b.stage, b.index, b.offset + 8);
            const uint64_t align = std::max<uint64_t>(16, g.props.limits.minStorageBufferOffsetAlignment);
            const uint64_t base = addr & ~(align - 1);
            BufRef r = addr ? buffer_at(base, 4) : BufRef{};
            if (r.buf) r.size = std::min<uint64_t>(r.size, size + (addr - base));
            if (b.written && r.buf) gpu_writes(base, r.size);
            if (trace_on)
                hos_log("vk trace:   storage binding %u: c%u+%#x -> %#llx size %u (%s)", b.binding, b.index, b.offset,
                        (unsigned long long)addr, size, r.buf ? "imported" : "none");
            bufs.push_back({r.buf, r.offset, r.buf ? std::max<uint64_t>(r.size, 4) : VK_WHOLE_SIZE});
            for (uint32_t k = 1; k < w.descriptorCount; k++) bufs.push_back(bufs.back());
            where.push_back({bufs.size() - w.descriptorCount, 0});
        } else if (b.kind == kTex) {
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            for (uint32_t k = 0; k < w.descriptorCount; k++) {
                const uint32_t h = descriptor_handle(s, b, k);
                VkDescriptorImageInfo ii{default_sampler(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
                nvn_tex t;
                nvn_view v;
                bool hv = false;
                const bool known = h && texture_of((int)(h & 0xFFFFF), &t, &v, &hv);
                if (known) {
                    if (Image* im = image_of(rp.cb, &t)) ii.imageView = view_of(im, &v, hv, b.tex_type, kViewSampled);
                    ii.sampler = sampler_of((int)(h >> 20));
                }
                if (trace_on && known && b.tex_type == 3)
                    hos_log("vk trace:   array texture: depth %u levels %u target %d; view %d: target %d/%d layers %d/%u+%u levels %d/%u+%u format %d/%#x",
                            t.d.depth, t.d.levels, t.d.target, hv, hv ? v.set_target : 0, hv ? v.target : 0, hv ? v.set_layers : 0,
                            hv ? v.min_layer : 0, hv ? v.nlayers : 0, hv ? v.set_levels : 0, hv ? v.base_level : 0, hv ? v.nlevels : 0,
                            hv ? v.set_format : 0, hv ? v.format : 0);
                if (trace_on)
                    hos_log("vk trace:   texture binding %u (type %d): c%u+%#x handle %#x -> %s %#llx %ux%u format %#x target %d, view %s", b.binding, b.tex_type,
                            b.index, b.offset, h, known ? "texture" : "nothing", known ? (unsigned long long)t.gpu : 0ull,
                            known ? t.d.width : 0, known ? t.d.height : 0, known ? t.d.format : 0, known ? t.d.target : 0,
                            ii.imageView ? "made" : "null");
                imgs.push_back(ii);
            }
            where.push_back({imgs.size() - w.descriptorCount, 1});
        } else if (b.kind == kImg) {   // (an image handle is its texture pool id: nvnTexturePoolRegisterImage)
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            for (uint32_t k = 0; k < w.descriptorCount; k++) {
                const uint32_t h = descriptor_handle(s, b, k);
                VkDescriptorImageInfo ii{VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
                nvn_tex t;
                nvn_view v;
                bool hv = false;
                if (h && texture_of((int)(h & 0xFFFFF), &t, &v, &hv))
                    if (Image* im = image_of(rp.cb, &t)) {
                        ii.imageView = view_of(im, &v, hv, b.tex_type, kViewStorage);
                        if (ii.imageView) rp.stored.push_back(im);
                    }
                if (trace_on)
                    hos_log("vk trace:   image binding %u: c%u+%#x handle %#x -> %s", b.binding, b.index, b.offset, h,
                            ii.imageView ? "bound" : "null");
                imgs.push_back(ii);
            }
            where.push_back({imgs.size() - w.descriptorCount, 1});
        } else {
            w.descriptorType = b.kind == kTexBuf ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            where.push_back({~(size_t)0, 2});
            vk_log_limited("texbuf", 4, "vk: a program uses texel buffers: not bound yet");
        }
        writes.push_back(w);
    }
    static const VkBufferView null_view = VK_NULL_HANDLE;
    std::vector<VkBufferView> views;
    for (size_t i = 0; i < writes.size(); i++) {
        if (where[i].second == 0) writes[i].pBufferInfo = &bufs[where[i].first];
        else if (where[i].second == 1) writes[i].pImageInfo = &imgs[where[i].first];
        else {
            views.assign(writes[i].descriptorCount, null_view);
            writes[i].pTexelBufferView = views.data();   // (null descriptors; one write of this kind at a time is enough here)
        }
    }
    if (!writes.empty()) g.x.vkCmdPushDescriptorSetKHR(rp.cb, point, p.layout, 0, (uint32_t)writes.size(), writes.data());
    uint8_t push[kPushBytes]{};
    const float down = 1.0f;
    memcpy(push + Shader::Backend::SPIRV::RESCALING_LAYOUT_DOWN_FACTOR_OFFSET, &down, 4);
    vkCmdPushConstants(rp.cb, p.layout, VK_SHADER_STAGE_ALL, 0, kPushBytes, push);
    return true;
}

/// indices for quads as triangles (0 1 2, 0 2 3 per quad), from the draw's indices or its vertex range
BufRef quad_indices(uint32_t count, const uint8_t* src, int itype, uint32_t first, uint32_t* out_count) {
    const uint32_t quads = count / 4;
    std::vector<uint32_t> idx(quads * 6);
    auto at = [&](uint32_t i) -> uint32_t {
        if (!src) return first + i;
        if (itype == NVN_INDEX_TYPE_UNSIGNED_BYTE) return src[i];
        if (itype == NVN_INDEX_TYPE_UNSIGNED_SHORT) return reinterpret_cast<const uint16_t*>(src)[i];
        return reinterpret_cast<const uint32_t*>(src)[i];
    };
    for (uint32_t q = 0; q < quads; q++) {
        const uint32_t a = at(4 * q), b = at(4 * q + 1), c = at(4 * q + 2), d = at(4 * q + 3);
        uint32_t* o = &idx[6 * q];
        o[0] = a, o[1] = b, o[2] = c, o[3] = a, o[4] = c, o[5] = d;
    }
    *out_count = quads * 6;
    return ring.put(idx.data(), idx.size() * 4, 4);
}

/// a draw's program inputs: the vertex attributes' types and the render targets' kinds
PipelineInputs draw_inputs(const State& s, int ncolor, const bool integer[8], const bool sint[8]) {
    PipelineInputs in{};
    for (int i = 0; i < 32; i++) in.attributes[i] = Shader::AttributeType::Disabled;
    for (int i = 0; i < 16; i++) {
        const nvn_va_st a = *reinterpret_cast<const nvn_va_st*>(&s.vattr[i]);
        if (a.fmt) in.attributes[i] = vertex_format((int)a.fmt).type;
    }
    for (int i = 0; i < 8; i++)
        in.color_outputs[i] = i < ncolor && integer[i] ? (sint[i] ? Shader::AttributeType::SignedInt : Shader::AttributeType::UnsignedInt)
                                                       : Shader::AttributeType::Float;
    in.convert_depth = false;   // (the depth clip range is set to -1..1 dynamically instead)
    return in;
}
PipelineInputs compute_inputs() {
    PipelineInputs in{};
    for (auto& a : in.attributes) a = Shader::AttributeType::Disabled;
    return in;
}

/// (the scan ahead of a submit's replay) the program a draw will need, made ahead when it is new
void draw_ahead(int prim, uint32_t count, uint32_t instances) {
    const State& s = *rp.s;
    bool ok_topology;
    topology(prim, &ok_topology);
    if (!s.render_enabled || !count || !instances || !ok_topology || !s.has_prog) return;
    const int n = std::clamp(s.rt.n, 0, 8);
    bool integer[8]{}, sint[8]{};
    for (int i = 0; i < n; i++)
        if (s.rt.color[i]) {
            const FormatInfo f = format_info(s.rt.color[i]->d.format);
            integer[i] = f.integer, sint[i] = f.sint;
        }
    const PipelineInputs in = draw_inputs(s, n, integer, sint);
    // (most draws use the program the draw before used)
    static nvn_prog last_prog;
    static uint64_t last_in;
    const uint64_t in_hash = inputs_hash(in);
    if (!memcmp(&last_prog, &s.prog, sizeof s.prog) && last_in == in_hash) return;
    last_prog = s.prog, last_in = in_hash;
    program_ahead(s, in, false);
}

void draw(int prim, bool indexed, uint32_t count, uint32_t first, int32_t base_vertex, uint32_t base_instance,
          uint32_t instances, uint64_t ibuf, int itype) {
    StatTimer timer{kStatDraw};
    State& s = *rp.s;
    if (!s.render_enabled || !s.cond_ok || !count || !instances) return;
    bool ok_topology;
    const VkPrimitiveTopology topo = topology(prim, &ok_topology);
    if (!ok_topology) {
        vk_log_limited("prim", 8, "vk: draw primitive %d: not drawn", prim);
        return;
    }
    // the program, for these vertex attributes and render targets
    if (!begin_rendering()) {
        rp.skipped++;
        return;
    }
    const PipelineInputs in = draw_inputs(s, rp.ncolor, rp.color_integer, rp.color_sint);
    if (!s.has_prog) return;
    Program* p = program_for(s, in, false);
    if (!p) {
        rp.skipped++;
        return;
    }
    const bool traced = trace_left();
    if (traced) {   // (which uniform buffer slots are bound, and which banks the program reads)
        char bound[256] = "", read[256] = "";
        for (int st = 0; st < 2; st++)
            for (int i = 0; i < 16; i++)
                if (s.st[st].ubo[i].addr)
                    snprintf(bound + strlen(bound), sizeof bound - strlen(bound), " %s%d(%llu)", st ? "fs" : "vs", i,
                             (unsigned long long)s.st[st].ubo[i].size);
        for (const Bind& b : p->binds)
            if (b.kind == kCbuf) snprintf(read + strlen(read), sizeof read - strlen(read), " %s:c%u", b.stage ? "fs" : "vs", b.index);
        hos_log("vk trace: draw: bound%s; reads%s", bound, read);
        char va[512] = "";
        for (int i = 0; i < 16; i++) {
            const nvn_va_st at = *reinterpret_cast<const nvn_va_st*>(&s.vattr[i]);
            if (at.fmt) snprintf(va + strlen(va), sizeof va - strlen(va), " a%d(f%#x s%u +%u)", i, (unsigned)at.fmt, (unsigned)at.stream, (unsigned)at.offset);
        }
        hos_log("vk trace: draw prim %d count %u: attributes%s; stream0 stride %u vbuf0 %#llx", prim, count, va,
                s.vstream[0].stride, (unsigned long long)s.vbuf[0].address);
        hos_log("vk trace:   viewport %g %g %g %g depth %g..%g scissor %d %d %d %d area %ux%u; depth test %d write %d func %d; "
                "cull %d front %d; blend %#x chmask %#x; discard %d enabled %d",
                s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3], s.depth_n, s.depth_f, s.scissor[0], s.scissor[1],
                s.scissor[2], s.scissor[3], rp.rw, rp.rh, (int)s.ds.dt, (int)s.ds.dw, (int)s.ds.df, (int)s.poly.cull,
                (int)s.poly.front, (unsigned)s.color.blend_en, s.chmask.mask, (int)s.raster_discard, (int)s.render_enabled);
        if (indexed) {
            uint8_t ib[8]{};
            nvn_gpu_read(ibuf, ib, 8);
            hos_log("vk trace:   indices type %d at %#llx (%s): %02x %02x %02x %02x %02x %02x %02x %02x", itype,
                    (unsigned long long)ibuf, buffer_at(ibuf, 1).buf ? "imported" : "not imported", ib[0], ib[1], ib[2], ib[3],
                    ib[4], ib[5], ib[6], ib[7]);
        }
        if (count <= 6) {
            float v[16]{};
            nvn_gpu_read(s.vbuf[0].address + (uint64_t)first * s.vstream[0].stride, v, sizeof v);
            hos_log("vk trace:   vertices %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g (first %u, vbuf size %llu)", v[0], v[1], v[2],
                    v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15], first,
                    (unsigned long long)s.vbuf[0].size);
        }
        hos_log("vk trace:   program %016llx", (unsigned long long)p->key);
        hos_log("vk trace:   blend0 %d %d %d / %d %d %d; program stages %zu; samples %d; ncolor %d formats %d",
                (int)s.blend[0].src_rgb, (int)s.blend[0].dst_rgb, (int)s.blend[0].eq_rgb, (int)s.blend[0].src_a,
                (int)s.blend[0].dst_a, (int)s.blend[0].eq_a, p->stages.size(), (int)rp.samples, rp.ncolor, (int)rp.color_formats[0]);
    }
    // descriptors (their textures may need filling: before_transfer ends the render pass, which is begun again)
    before_transfer = before_transfer_hook;
    trace_on = traced;
    rp.stored.clear();
    push_descriptors(*p, VK_PIPELINE_BIND_POINT_GRAPHICS);
    for (Image* im : rp.stored) image_written(im);
    trace_on = false;
    before_transfer = nullptr;
    if (!begin_rendering()) return;
    VkCommandBuffer cb = rp.cb;
    // shaders (the stages a program has not: none)
    std::vector<VkShaderStageFlagBits> stages = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    if (g.tessellation) stages.push_back(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT), stages.push_back(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
    if (g.geometry) stages.push_back(VK_SHADER_STAGE_GEOMETRY_BIT);
    std::vector<VkShaderEXT> shaders(stages.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < stages.size(); i++)
        for (size_t k = 0; k < p->stages.size(); k++)
            if (p->stages[k] == stages[i]) shaders[i] = p->shaders[k];
    g.x.vkCmdBindShadersEXT(cb, (uint32_t)stages.size(), stages.data(), shaders.data());
    // viewport and scissor
    VkViewport vp{s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3], s.depth_n, s.depth_f};
    if (vp.width == 0 || vp.height == 0) vp.width = (float)rp.rw, vp.height = (float)rp.rh;
    // NVN's viewport transform puts clip-space +y at the top of the window (the game's projections are GL's, y up) and
    // its window origin is the upper left (row 0 at the top of the image): a y-flipped Vulkan viewport. The flip also
    // keeps NVN's sense of a front face (Vulkan judges winding in its y-down framebuffer).
    vp.y += vp.height, vp.height = -vp.height;
    vp.minDepth = std::clamp(vp.minDepth, 0.0f, 1.0f), vp.maxDepth = std::clamp(vp.maxDepth, 0.0f, 1.0f);
    vkCmdSetViewportWithCount(cb, 1, &vp);
    const VkRect2D sc = scissor_rect();
    vkCmdSetScissorWithCount(cb, 1, &sc);
    // rasterization
    vkCmdSetRasterizerDiscardEnable(cb, s.raster_discard);
    vkCmdSetCullMode(cb, s.poly.cull == 1 ? VK_CULL_MODE_FRONT_BIT : s.poly.cull == 2 ? VK_CULL_MODE_BACK_BIT
                         : s.poly.cull == 3 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cb, s.poly.front == NVN_FRONT_FACE_CW ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
    g.x.vkCmdSetPolygonModeEXT(cb, s.poly.mode == 0 ? VK_POLYGON_MODE_POINT : s.poly.mode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL);
    vkCmdSetDepthBiasEnable(cb, (s.poly.offset_en & 4) != 0);
    vkCmdSetDepthBias(cb, s.poly_offset[1], s.poly_offset[2], s.poly_offset[0]);
    vkCmdSetLineWidth(cb, 1.0f);
    g.x.vkCmdSetDepthClampEnableEXT(cb, s.depth_clamp);
    if (g.x.vkCmdSetDepthClipEnableEXT) g.x.vkCmdSetDepthClipEnableEXT(cb, !s.depth_clamp);
    g.x.vkCmdSetDepthClipNegativeOneToOneEXT(cb, nvn_depth_minus_w() != 0);
    // multisampling
    g.x.vkCmdSetRasterizationSamplesEXT(cb, rp.samples);
    const VkSampleMask mask = ~0u;
    g.x.vkCmdSetSampleMaskEXT(cb, rp.samples, &mask);
    g.x.vkCmdSetAlphaToCoverageEnableEXT(cb, s.ms.a2c);
    g.x.vkCmdSetAlphaToOneEnableEXT(cb, VK_FALSE);
    // depth and stencil
    vkCmdSetDepthTestEnable(cb, rp.has_depth && s.ds.dt);
    vkCmdSetDepthWriteEnable(cb, rp.has_depth && s.ds.dw);
    vkCmdSetDepthCompareOp(cb, compare_op((int)s.ds.df));
    vkCmdSetDepthBoundsTestEnable(cb, VK_FALSE);
    vkCmdSetStencilTestEnable(cb, rp.has_stencil && s.ds.st);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, stencil_op((int)s.ds.fail_f), stencil_op((int)s.ds.zpass_f),
                      stencil_op((int)s.ds.zfail_f), compare_op((int)s.ds.sf_f));
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, stencil_op((int)s.ds.fail_b), stencil_op((int)s.ds.zpass_b),
                      stencil_op((int)s.ds.zfail_b), compare_op((int)s.ds.sf_b));
    for (int f = 0; f < 2; f++) {
        const VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
        vkCmdSetStencilCompareMask(cb, face, (uint32_t)s.stencil_value_mask[f]);
        vkCmdSetStencilWriteMask(cb, face, (uint32_t)s.stencil_mask[f]);
        vkCmdSetStencilReference(cb, face, (uint32_t)s.stencil_ref[f]);
    }
    // color
    const uint32_t nc = (uint32_t)std::max(rp.ncolor, 1);
    VkBool32 enables[8]{};
    VkColorBlendEquationEXT eqs[8]{};
    VkColorComponentFlags writes[8]{};
    for (uint32_t i = 0; i < nc; i++) {
        const nvn_blend_st& b = s.blend[i];
        enables[i] = (s.color.blend_en >> i & 1) && i < (uint32_t)rp.ncolor && !rp.color_integer[i];
        eqs[i] = {blend_factor((int)b.src_rgb), blend_factor((int)b.dst_rgb), blend_op((int)b.eq_rgb),
                  blend_factor((int)b.src_a), blend_factor((int)b.dst_a), blend_op((int)b.eq_a)};
        writes[i] = (s.chmask.mask >> (4 * i)) & 0xf;
        if (i >= (uint32_t)rp.ncolor || !rp.color_formats[i]) writes[i] = 0;
    }
    g.x.vkCmdSetColorBlendEnableEXT(cb, 0, nc, enables);
    g.x.vkCmdSetColorBlendEquationEXT(cb, 0, nc, eqs);
    g.x.vkCmdSetColorWriteMaskEXT(cb, 0, nc, writes);
    vkCmdSetBlendConstants(cb, s.blend_color);
    g.x.vkCmdSetLogicOpEnableEXT(cb, VK_FALSE);
    if (g.x.vkCmdSetLogicOpEXT) g.x.vkCmdSetLogicOpEXT(cb, VK_LOGIC_OP_COPY);
    // primitives and vertices
    vkCmdSetPrimitiveTopology(cb, topo);
    vkCmdSetPrimitiveRestartEnable(cb, s.restart && indexed &&
                                           topo != VK_PRIMITIVE_TOPOLOGY_POINT_LIST && topo != VK_PRIMITIVE_TOPOLOGY_LINE_LIST &&
                                           topo != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    std::vector<VkVertexInputBindingDescription2EXT> vbind;
    std::vector<VkVertexInputAttributeDescription2EXT> vattr;
    bool stream_used[16]{};
    for (int i = 0; i < 16; i++) {
        const nvn_va_st a = *reinterpret_cast<const nvn_va_st*>(&s.vattr[i]);
        if (!a.fmt || (p->loads_generic.size() > (size_t)i && !p->loads_generic[i])) continue;
        const VertexFormat f = vertex_format((int)a.fmt);
        if (!f.vk) continue;
        VkVertexInputAttributeDescription2EXT d{VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT};
        d.location = (uint32_t)i, d.binding = a.stream, d.format = f.vk, d.offset = a.offset;
        vattr.push_back(d);
        stream_used[a.stream & 15] = true;
    }
    // (inputs the shader reads that no attribute feeds: zeros, from a buffer of its own)
    static BufRef zeros;
    bool need_zero = false;
    for (size_t i = 0; i < p->loads_generic.size() && i < 16; i++) {
        if (!p->loads_generic[i]) continue;
        bool fed = false;
        for (const auto& a : vattr) fed |= a.location == i;
        if (fed) continue;
        VkVertexInputAttributeDescription2EXT d{VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT};
        d.location = (uint32_t)i, d.binding = 16, d.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        vattr.push_back(d);
        need_zero = true;
    }
    for (uint32_t i = 0; i < 16; i++) {
        if (!stream_used[i]) continue;
        VkVertexInputBindingDescription2EXT d{VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT};
        d.binding = i, d.stride = s.vstream[i].stride, d.divisor = 1;
        d.inputRate = s.vstream[i].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
        vbind.push_back(d);
        const BufRef r = s.vbuf[i].address ? buffer_at(s.vbuf[i].address, 1) : BufRef{};
        if (r.buf) {
            const VkDeviceSize size = std::min<uint64_t>(r.size, std::max<uint64_t>(s.vbuf[i].size, 1));
            vkCmdBindVertexBuffers2(cb, i, 1, &r.buf, &r.offset, &size, nullptr);
        } else {
            vk_log_limited("vbuf", 16, "vk: vertex buffer %#llx (+%#llx) is not in an imported pool: zeros", (unsigned long long)s.vbuf[i].address,
                           (unsigned long long)s.vbuf[i].size);
            const BufRef z = ring.put(nullptr, 64, 16);
            vkCmdBindVertexBuffers2(cb, i, 1, &z.buf, &z.offset, nullptr, nullptr);
        }
    }
    if (need_zero) {
        zeros = ring.put(nullptr, 64, 16);
        VkVertexInputBindingDescription2EXT d{VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT};
        d.binding = 16, d.stride = 0, d.inputRate = VK_VERTEX_INPUT_RATE_VERTEX, d.divisor = 1;
        vbind.push_back(d);
        vkCmdBindVertexBuffers2(cb, 16, 1, &zeros.buf, &zeros.offset, nullptr, nullptr);
    }
    g.x.vkCmdSetVertexInputEXT(cb, (uint32_t)vbind.size(), vbind.data(), (uint32_t)vattr.size(), vattr.data());
    // the draw
    if (prim == NVN_DRAW_PRIMITIVE_QUADS) {
        std::vector<uint8_t> idx;
        const uint32_t isize = itype == NVN_INDEX_TYPE_UNSIGNED_BYTE ? 1 : itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? 2 : 4;
        if (indexed) {
            idx.resize((size_t)count * isize);
            if (!nvn_gpu_read(ibuf, idx.data(), idx.size())) return;
        }
        uint32_t n = 0;
        const BufRef q = quad_indices(count, indexed ? idx.data() : nullptr, itype, first, &n);
        vkCmdBindIndexBuffer(cb, q.buf, q.offset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, n, instances, 0, indexed ? base_vertex : 0, base_instance);
    } else if (indexed) {
        const BufRef r = buffer_at(ibuf, 1);
        const VkIndexType t = itype == NVN_INDEX_TYPE_UNSIGNED_BYTE ? VK_INDEX_TYPE_UINT8_EXT
                              : itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        if (r.buf) {
            vkCmdBindIndexBuffer(cb, r.buf, r.offset, t);
        } else {
            const uint32_t isize = t == VK_INDEX_TYPE_UINT8_EXT ? 1 : t == VK_INDEX_TYPE_UINT16 ? 2 : 4;
            std::vector<uint8_t> idx((size_t)count * isize);
            if (!nvn_gpu_read(ibuf, idx.data(), idx.size())) return;
            const BufRef c = ring.put(idx.data(), idx.size(), 4);
            vkCmdBindIndexBuffer(cb, c.buf, c.offset, t);
        }
        vkCmdDrawIndexed(cb, count, instances, 0, base_vertex, base_instance);
    } else {
        vkCmdDraw(cb, count, instances, first, base_instance);
    }
    rp.draws++;
}

// ---- compute ----
/// a dispatch of the bound program's compute stage: x, y, z groups, or (indirect, when not 0) the three 32-bit counts
/// there, which the GPU may have written earlier in this submit
/// (the scan ahead) the program a dispatch will need
void dispatch_ahead(uint32_t x, uint32_t y, uint32_t z, uint64_t indirect) {
    State& s = *rp.s;
    if (!s.has_prog) return;
    if (indirect) {
        uint32_t n[3]{};
        nvn_gpu_read(indirect, n, sizeof n);
        x = n[0], y = n[1], z = n[2];
    } else if (!x || !y || !z) {
        return;
    }
    s.grid[0] = x, s.grid[1] = y, s.grid[2] = z;
    program_ahead(s, compute_inputs(), true);
}

void dispatch(uint32_t x, uint32_t y, uint32_t z, uint64_t indirect) {
    State& s = *rp.s;
    if (!s.has_prog) return;
    BufRef ind{};
    if (indirect) {   // (the counts as they are now, for the driver bank's grid; the GPU reads them itself)
        uint32_t n[3]{};
        nvn_gpu_read(indirect, n, sizeof n);
        x = n[0], y = n[1], z = n[2];
        ind = buffer_at(indirect, 12);
    } else if (!x || !y || !z) {
        return;
    }
    end_rendering();
    s.grid[0] = x, s.grid[1] = y, s.grid[2] = z;
    Program* p = program_for(s, compute_inputs(), true);
    if (!p) {
        rp.skipped++;
        return;
    }
    const bool traced = trace_left();
    if (traced)
        hos_log("vk trace: dispatch %u x %u x %u%s, program %016llx", x, y, z, indirect ? " (indirect)" : "",
                (unsigned long long)p->key);
    else
        vk_log_limited("dispatch", 4, "vk: compute dispatch %u x %u x %u%s (program %016llx)", x, y, z,
                       indirect ? ", indirect" : "", (unsigned long long)p->key);
    full_barrier(rp.cb);   // (what came before: its writes are the dispatch's inputs; image fills, too)
    trace_on = traced;
    rp.stored.clear();
    push_descriptors(*p, VK_PIPELINE_BIND_POINT_COMPUTE);
    trace_on = false;
    const VkShaderStageFlagBits stage = VK_SHADER_STAGE_COMPUTE_BIT;
    g.x.vkCmdBindShadersEXT(rp.cb, 1, &stage, p->shaders.data());
    if (indirect && ind.buf && !(ind.offset & 3)) vkCmdDispatchIndirect(rp.cb, ind.buf, ind.offset);
    else if (x && y && z) vkCmdDispatch(rp.cb, x, y, z);
    full_barrier(rp.cb);   // (its writes, for what comes after)
    for (Image* im : rp.stored) image_written(im);
    rp.dispatches++;
}

// ---- copies ----
VkImageAspectFlags aspect_of(Image* im) {
    const FormatInfo& f = image_format_info(im);
    return f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : f.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

void copy_buffer_texture(const nvn_p_copy_bt& c, bool to_texture) {
    end_rendering();
    Image* im = image_of(rp.cb, c.tex);
    if (!im) return;
    const unsigned level = c.has_view && c.view.set_levels ? c.view.base_level : 0;
    const unsigned layer = c.has_view && c.view.set_layers ? c.view.min_layer : 0;
    int bw = 1, bh = 1;
    const int bpb = nvn_fmt_bpb(c.tex->d.format, &bw, &bh);
    if (bpb <= 0 || image_format_info(im).astc) return;
    const BufRef r = buffer_at(c.buf, 1);
    if (!r.buf) {
        vk_log_limited("copybt", 8, "vk: a buffer/texture copy at %#llx is not in an imported pool", (unsigned long long)c.buf);
        return;
    }
    VkBufferImageCopy bc{};
    bc.bufferOffset = r.offset;
    if (rp.s->copy_stride > 0) bc.bufferRowLength = (uint32_t)(rp.s->copy_stride / bpb * bw);
    const bool is3d = c.tex->d.target == NVN_TEXTURE_TARGET_3D;
    const bool arrayed = !is3d && image_layers(im) > 1;
    bc.imageSubresource = {aspect_of(im), level, layer + (arrayed ? (uint32_t)c.r.zoffset : 0u),
                           arrayed ? (uint32_t)std::max(1, c.r.depth) : 1u};
    bc.imageOffset = {c.r.xoffset, c.r.yoffset, is3d ? c.r.zoffset : 0};
    bc.imageExtent = {(uint32_t)std::max(1, c.r.width), (uint32_t)std::max(1, c.r.height), is3d ? (uint32_t)std::max(1, c.r.depth) : 1u};
    if (to_texture) {
        vkCmdCopyBufferToImage(rp.cb, r.buf, image_handle(im), VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    } else {
        rp.writes_unknown = true;   // (rows, layers and blocks: not worth the exact extent)
        vkCmdCopyImageToBuffer(rp.cb, image_handle(im), VK_IMAGE_LAYOUT_GENERAL, r.buf, 1, &bc);
    }
    full_barrier(rp.cb);
}

void copy_texture_texture(const nvn_p_copy_tt& c) {
    end_rendering();
    Image* src = image_of(rp.cb, c.src);
    Image* dst = image_of(rp.cb, c.dst);
    if (!src || !dst) return;
    const unsigned sl = c.has_sv && c.sv.set_levels ? c.sv.base_level : 0, dl = c.has_dv && c.dv.set_levels ? c.dv.base_level : 0;
    const unsigned sa = c.has_sv && c.sv.set_layers ? c.sv.min_layer : 0, da = c.has_dv && c.dv.set_layers ? c.dv.min_layer : 0;
    const bool s3 = c.src->d.target == NVN_TEXTURE_TARGET_3D, d3 = c.dst->d.target == NVN_TEXTURE_TARGET_3D;
    VkImageBlit b{};
    b.srcSubresource = {aspect_of(src), sl, sa + (s3 ? 0u : (uint32_t)c.sr.zoffset), 1};
    b.dstSubresource = {aspect_of(dst), dl, da + (d3 ? 0u : (uint32_t)c.dr.zoffset), 1};
    b.srcOffsets[0] = {c.sr.xoffset, c.sr.yoffset, s3 ? c.sr.zoffset : 0};
    b.srcOffsets[1] = {c.sr.xoffset + c.sr.width, c.sr.yoffset + c.sr.height, (s3 ? c.sr.zoffset : 0) + 1};
    b.dstOffsets[0] = {c.dr.xoffset, c.dr.yoffset, d3 ? c.dr.zoffset : 0};
    b.dstOffsets[1] = {c.dr.xoffset + c.dr.width, c.dr.yoffset + c.dr.height, (d3 ? c.dr.zoffset : 0) + 1};
    const bool same_size = c.sr.width == c.dr.width && c.sr.height == c.dr.height;
    const FormatInfo &fs = image_format_info(src), &fd = image_format_info(dst);
    if (same_size && (fs.compressed || fd.compressed || fs.integer || fd.integer || fs.depth)) {
        VkImageCopy ic{};
        ic.srcSubresource = b.srcSubresource, ic.dstSubresource = b.dstSubresource;
        ic.srcOffset = b.srcOffsets[0], ic.dstOffset = b.dstOffsets[0];
        ic.extent = {(uint32_t)c.sr.width, (uint32_t)c.sr.height, 1};
        vkCmdCopyImage(rp.cb, image_handle(src), VK_IMAGE_LAYOUT_GENERAL, image_handle(dst), VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    } else {
        vkCmdBlitImage(rp.cb, image_handle(src), VK_IMAGE_LAYOUT_GENERAL, image_handle(dst), VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       (c.flags & 1) && !fs.integer ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    image_written(dst);
    full_barrier(rp.cb);
}

void clear_texture(const nvn_p_clear_tex& c) {
    end_rendering();
    Image* im = image_of(rp.cb, c.tex);
    if (!im) return;
    const unsigned level = c.has_view && c.view.set_levels ? c.view.base_level : 0;
    const unsigned layer = c.has_view && c.view.set_layers ? c.view.min_layer : 0;
    VkImageSubresourceRange range{aspect_of(im), level, 1, layer, 1};
    if (image_format_info(im).depth || image_format_info(im).stencil) {
        VkClearDepthStencilValue v{c.c.f[0], c.c.u[1]};
        vkCmdClearDepthStencilImage(rp.cb, image_handle(im), VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
    } else {
        VkClearColorValue v;
        memcpy(&v, c.c.u, 16);
        vkCmdClearColorImage(rp.cb, image_handle(im), VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
    }
    image_written(im);
    full_barrier(rp.cb);
}

void copy_b2b(const nvn_p_copy_bb& c) {
    const BufRef s = buffer_at(c.src, c.size), d = buffer_at(c.dst, c.size);
    if (!c.size) return;
    end_rendering();
    if (s.buf && d.buf) {
        VkBufferCopy bc{s.offset, d.offset, c.size};
        vkCmdCopyBuffer(rp.cb, s.buf, d.buf, 1, &bc);
        gpu_writes(c.dst, c.size);
        full_barrier(rp.cb);
    } else {
        vk_log_limited("b2b", 8, "vk: a buffer copy outside imported pools: done by the CPU, out of order");
        std::vector<uint8_t> tmp(c.size);
        if (nvn_gpu_read(c.src, tmp.data(), c.size)) nvn_gpu_write(c.dst, tmp.data(), c.size);
    }
}

void update_ub(const nvn_p_update_ub& u, const uint8_t* data) {
    if (!u.size) return;
    const BufRef d = buffer_at(u.dst, u.size);
    if (!d.buf || (d.offset & 3) || (u.size & 3) || u.size > 65536) {
        nvn_gpu_write(u.dst, data, u.size);   // (out of order with the GPU's reads: rare)
        return;
    }
    end_rendering();
    vkCmdUpdateBuffer(rp.cb, d.buf, d.offset, u.size, data);
    gpu_writes(u.dst, u.size);
    full_barrier(rp.cb);
}

void clear_buf(const nvn_p_clear_buf& c) {
    const BufRef d = buffer_at(c.dst, c.size);
    if (!c.size) return;
    if (!d.buf || (d.offset & 3) || (c.size & 3)) {
        std::vector<uint32_t> v((c.size + 3) / 4, c.value);
        nvn_gpu_write(c.dst, v.data(), c.size);
        return;
    }
    end_rendering();
    vkCmdFillBuffer(rp.cb, d.buf, d.offset, c.size, c.value);
    gpu_writes(c.dst, c.size);
    full_barrier(rp.cb);
}

// ---- the CPU's side, after the GPU ----
/// what a CPU op writes: the bytes at *addr, and how many (0: nothing); for an event's add, bytes holds the old value
size_t cpu_op_write(const CpuOp& o, uint64_t* addr, uint8_t* bytes, bool log) {
    if (o.op == NOP_SIGNAL_EVENT) {
        nvn_p_signal sg;
        memcpy(&sg, o.bytes, sizeof sg);
        uint32_t v = 0;
        if (sg.mode == NVN_EVENT_SIGNAL_MODE_ADD) memcpy(&v, bytes, 4);
        v = sg.mode == NVN_EVENT_SIGNAL_MODE_ADD ? v + sg.value : sg.value;
        *addr = sg.gpu;
        memcpy(bytes, &v, 4);
        return 4;
    }
    nvn_p_counter r;
    memcpy(&r, o.bytes, sizeof r);
    NVNcounterData d = {0, 0};
    const uint32_t type = (uint32_t)r.type;
    if (static uint32_t seen; log && type < 32 && !(__atomic_fetch_or(&seen, 1u << type, __ATOMIC_RELAXED) & (1u << type)))
        hos_log("vk: the game reports counter type %u (first at %.1f s)", type, hos_now_ns() / 1e9);
    if (type == NVN_COUNTER_TYPE_TIMESTAMP || type == NVN_COUNTER_TYPE_TIMESTAMP_TOP) d.timestamp = a64_cntpct_host();
    if (r.type & NVN_COUNTER_WRITTEN) return 0;
    *addr = r.addr;
    memcpy(bytes, &d, sizeof d);
    return sizeof d;
}

void cpu_ops() {
    for (const CpuOp& o : rp.cpu) {
        uint64_t addr = 0;
        uint8_t bytes[sizeof(NVNcounterData)];
        if (o.op == NOP_SIGNAL_EVENT) {   // (an add reads the value first)
            nvn_p_signal sg;
            memcpy(&sg, o.bytes, sizeof sg);
            nvn_gpu_read(sg.gpu, bytes, 4);
        }
        if (const size_t n = cpu_op_write(o, &addr, bytes, true)) nvn_gpu_write(addr, bytes, n);
    }
    rp.cpu.clear();
}

/// guest memory [gpu, gpu + size) as it will be once the CPU ops recorded so far are done
void read_after_cpu_ops(uint64_t gpu, uint8_t* out, size_t size) {
    nvn_gpu_read(gpu, out, size);
    for (const CpuOp& o : rp.cpu) {
        uint64_t addr = 0;
        uint8_t bytes[sizeof(NVNcounterData)]{};
        if (o.op == NOP_SIGNAL_EVENT) {
            nvn_p_signal sg;
            memcpy(&sg, o.bytes, sizeof sg);
            nvn_gpu_read(sg.gpu, bytes, 4);   // (the value it adds to: what the ops before it left in out)
            for (uint64_t i = 0; i < 4; i++)
                if (sg.gpu + i >= gpu && sg.gpu + i < gpu + size) bytes[i] = out[sg.gpu + i - gpu];
        }
        const size_t n = cpu_op_write(o, &addr, bytes, false);
        for (size_t i = 0; i < n; i++)
            if (addr + i >= gpu && addr + i < gpu + size) out[addr + i - gpu] = bytes[i];
    }
}

/// HOS_VK_PASSDUMP=<dir>@<seconds> (debugging): for one frame from that time on, each render pass's targets once it
/// ends, <dir>/<n>_c<i>_<address>.bmp (the replay's command buffer is run to that point for each)
struct PassDump {
    std::string dir;
    int64_t at = -1;
    int state = 0;   // 0 off, 1 waiting for the frame to start, 2 dumping, 3 done
    int n = 0, frames = 0;
} pd;
void pass_dump(const State& s) {
    if (pd.at == -1) {
        pd.at = -2;
        if (const char* e = getenv("HOS_VK_PASSDUMP"); e && strchr(e, '@')) {
            pd.dir.assign(e, strchr(e, '@'));
            pd.at = (int64_t)(atof(strchr(e, '@') + 1) * 1e9);
            pd.state = 1;
        }
    }
    if (pd.state != 2) return;
    const int n = std::clamp(s.rt.n, 0, 8);
    bool any = false;
    for (int i = 0; i < n; i++) any |= s.rt.color[i] != nullptr;
    if (!any && !s.rt.depth) return;
    // (run what is recorded so far, then save)
    vkEndCommandBuffer(rp.cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1, si.pCommandBuffers = &rp.cb;
    vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g.queue);
    vkFreeCommandBuffers(g.dev, g.pool, 1, &rp.cb);
    for (int i = 0; i < n; i++) {
        Image* im = s.rt.color[i] ? image_find(s.rt.color[i]) : nullptr;
        if (!im) continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%04d_c%d_%llx_%ux%u_f%x.bmp", pd.dir.c_str(), pd.n, i, (unsigned long long)s.rt.color[i]->gpu,
                 s.rt.color[i]->d.width, s.rt.color[i]->d.height, s.rt.color[i]->d.format);
        uint32_t w, h;
        image_extent(im, 0, &w, &h);
        if (!image_format_info(im).integer) save_image_bmp(image_handle(im), image_format(im), w, h, path);
    }
    if (Image* im = s.rt.depth ? image_find(s.rt.depth) : nullptr; im && image_format_info(im).depth && !image_format_info(im).stencil) {
        char path[512];
        snprintf(path, sizeof path, "%s/%04d_depth_%llx.bmp", pd.dir.c_str(), pd.n, (unsigned long long)s.rt.depth->gpu);
        dump_depth(im, path);
    }
    pd.n++;
    rp.cb = begin_commands();
}
void pass_dump_frame_end() {   // (at a present)
    if (pd.state == 1 && hos_now_ns() >= pd.at) pd.state = 2;
    else if (pd.state == 2 && ++pd.frames == 2) pd.state = 3;   // (two frames: a frame's window pass runs after its present)
}

void cpu_ops();
/// runs what is recorded so far to completion, and the CPU's side of it, and goes on in a new command buffer
void flush() {
    end_rendering();
    end_and_wait(rp.cb);
    staging_release();
    ring.reset();
    cpu_ops();
    rp.writes.clear(), rp.writes_unknown = false;
    rp.cb = begin_commands();
}

/// SetRenderEnableConditional: rendering (draws and clears) only while the counters at addr and addr + 16 are equal
/// (mode 0) or differ (mode 1). Read when the GPU's work before it has run, as the GPU reads them at that point.
///
/// The work recorded before it is run first (a submit and a wait for the GPU) only when it may write them: the UI tests
/// a condition for each layer, ~100 a frame, against counters the game's CPU writes or reports just before (the reports'
/// values are ours, applied here as cpu_ops will apply them). HOS_VK_COND_CHECK=1 runs the work every time and logs
/// where the two reads differ (none in 160k on the island).
void render_cond(const nvn_p_render_cond& c) {
    static const bool check = getenv("HOS_VK_COND_CHECK") && atoi(getenv("HOS_VK_COND_CHECK"));
    uint64_t a = 0, b = 0;
    if (gpu_may_write(c.addr, 24)) flush();
    uint8_t v[24];
    read_after_cpu_ops(c.addr, v, sizeof v);
    memcpy(&a, v, 8);
    memcpy(&b, v + 16, 8);
    if (check) {   // (the GPU's work run after all, and read again)
        flush();
        uint64_t a1 = 0, b1 = 0;
        nvn_gpu_read(c.addr, &a1, 8);
        nvn_gpu_read(c.addr + 16, &b1, 8);
        if (a1 != a || b1 != b)
            hos_log("vk: HOS_VK_COND_CHECK: the condition at %#llx read %llu, %llu before the GPU's work and %llu, %llu after",
                    (unsigned long long)c.addr, (unsigned long long)a, (unsigned long long)b, (unsigned long long)a1,
                    (unsigned long long)b1);
        a = a1, b = b1;
    }
    rp.s->cond_ok = c.mode == 0 ? a == b : a != b;
    if (trace_left()) hos_log("vk trace: render if %s: %llu, %llu -> %s", c.mode ? "differ" : "equal", (unsigned long long)a,
                              (unsigned long long)b, rp.s->cond_ok ? "on" : "off");
}

/// (the scan ahead of a submit's replay) an op that is not only state: its program made ahead for a draw or dispatch,
/// else skipped; false for the ops that only set state, which the scan follows as the replay does
bool scan_op(State& s, uint32_t op, const uint8_t* a) {
    switch (op) {
    case NOP_SET_RT: memcpy(&s.rt, a, sizeof s.rt); return true;
    case NOP_DRAW: {
        const auto* d = reinterpret_cast<const nvn_p_draw*>(a);
        draw_ahead(d->prim, (uint32_t)d->count, (uint32_t)d->instances);
        return true;
    }
    case NOP_DRAW_IDX: {
        const auto* d = reinterpret_cast<const nvn_p_draw_idx*>(a);
        draw_ahead(d->prim, (uint32_t)d->count, (uint32_t)d->instances);
        return true;
    }
    case NOP_DISPATCH: {
        const auto* d = reinterpret_cast<const nvn_p_dispatch*>(a);
        dispatch_ahead((uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z, 0);
        return true;
    }
    case NOP_DISPATCH_INDIRECT: dispatch_ahead(0, 0, 0, reinterpret_cast<const nvn_p_dispatch_indirect*>(a)->addr); return true;
    case NOP_SIGNAL_EVENT: case NOP_REPORT_COUNTER: case NOP_COPY_B2B: case NOP_UPDATE_UB: case NOP_CLEAR_BUF:
    case NOP_CLEAR_COLOR: case NOP_CLEAR_DS: case NOP_RENDER_COND: case NOP_DRAW_INDIRECT: case NOP_WAIT_EVENT:
    case NOP_BARRIER: case NOP_COPY_B2T: case NOP_COPY_T2B: case NOP_COPY_T2T: case NOP_CLEAR_TEX:
        return true;
    default: return false;
    }
}

void run(const uint8_t* p) {
    State& s = *rp.s;
    for (;;) {
        nvn_op_hdr h;
        memcpy(&h, p, sizeof h);
        const uint8_t* a = p + 8;
        if (rp.scanning && h.op != NOP_END && scan_op(s, h.op, a)) {
            p += h.size;
            continue;
        }
        switch (h.op) {
        case NOP_END: return;
        case NOP_SIGNAL_EVENT: case NOP_REPORT_COUNTER: {
            CpuOp o{(int)h.op, {}};
            memcpy(o.bytes, a, h.op == NOP_SIGNAL_EVENT ? sizeof(nvn_p_signal) : sizeof(nvn_p_counter));
            rp.cpu.push_back(o);
            break;
        }
        case NOP_COPY_B2B: copy_b2b(*reinterpret_cast<const nvn_p_copy_bb*>(a)); break;
        case NOP_UPDATE_UB: {
            const auto* u = reinterpret_cast<const nvn_p_update_ub*>(a);
            update_ub(*u, a + sizeof *u);
            break;
        }
        case NOP_CLEAR_BUF: clear_buf(*reinterpret_cast<const nvn_p_clear_buf*>(a)); break;
        case NOP_SET_RT: {
            end_rendering();
            pass_dump(s);
            static uint64_t draws_at;
            if (trace_left()) {
                hos_log("vk trace: %llu draws, %llu skipped before; targets now:", (unsigned long long)(rp.draws - draws_at),
                        (unsigned long long)rp.skipped);
                const auto* r = reinterpret_cast<const nvn_p_set_rt*>(a);
                for (int i = 0; i < r->n && i < 8; i++)
                    if (r->color[i])
                        hos_log("vk trace:   color %d: %#llx %ux%u format %#x target %d", i, (unsigned long long)r->color[i]->gpu,
                                r->color[i]->d.width, r->color[i]->d.height, r->color[i]->d.format, r->color[i]->d.target);
                if (r->depth)
                    hos_log("vk trace:   depth: %#llx %ux%u format %#x", (unsigned long long)r->depth->gpu, r->depth->d.width,
                            r->depth->d.height, r->depth->d.format);
            }
            draws_at = rp.draws;
            memcpy(&s.rt, a, sizeof s.rt);
            break;
        }
        case NOP_CLEAR_COLOR: clear_color(*reinterpret_cast<const nvn_p_clear_color*>(a)); break;
        case NOP_CLEAR_DS: clear_ds(*reinterpret_cast<const nvn_p_clear_ds*>(a)); break;
        case NOP_VIEWPORTS: {
            const auto* v = reinterpret_cast<const nvn_p_viewports*>(a);
            if (v->first == 0 && v->n > 0) memcpy(s.viewport, v->v[0], 16);
            break;
        }
        case NOP_SCISSOR: {
            const auto* v = reinterpret_cast<const nvn_p_scissor*>(a);
            s.scissor[0] = v->x, s.scissor[1] = v->y, s.scissor[2] = v->w, s.scissor[3] = v->h;
            break;
        }
        case NOP_DEPTH_RANGE: {
            const auto* v = reinterpret_cast<const nvn_p_depth_range*>(a);
            s.depth_n = v->n, s.depth_f = v->f;
            break;
        }
        case NOP_BIND_PROGRAM: {
            const auto* b = reinterpret_cast<const nvn_p_bind_program*>(a);
            if (b->prog) s.prog = *b->prog, s.has_prog = true;
            break;
        }
        case NOP_BIND_STATE: {
            const auto* b = reinterpret_cast<const nvn_p_bind_state*>(a);
            switch (b->kind) {
            case 0: {
                nvn_blend_st bl;
                memcpy(&bl, b->bytes, sizeof bl);
                s.blend[bl.target & 7] = bl;
                break;
            }
            case 1: memcpy(&s.color, b->bytes, sizeof s.color); break;
            case 2: memcpy(&s.chmask, b->bytes, sizeof s.chmask); break;
            case 3: memcpy(&s.ms, b->bytes, sizeof s.ms); break;
            case 4: memcpy(&s.poly, b->bytes, sizeof s.poly); break;
            case 5: memcpy(&s.ds, b->bytes, sizeof s.ds); break;
            default: break;
            }
            break;
        }
        case NOP_BIND_VATTR: {
            const auto* b = reinterpret_cast<const nvn_p_bind_vattr*>(a);
            for (int i = 0; i < 16; i++) s.vattr[i] = 0;
            for (int i = 0; i < b->n && b->first + i < 16; i++) s.vattr[b->first + i] = b->st[i];
            break;
        }
        case NOP_BIND_VSTREAM: {
            const auto* b = reinterpret_cast<const nvn_p_bind_vstream*>(a);
            for (int i = 0; i < b->n && b->first + i < 16; i++) s.vstream[b->first + i] = b->st[i];
            break;
        }
        case NOP_BIND_VBUF: {
            const auto* b = reinterpret_cast<const nvn_p_bind_vbuf*>(a);
            for (int i = 0; i < b->n && b->first + i < 16; i++) s.vbuf[b->first + i] = b->r[i];
            break;
        }
        case NOP_BIND_UBO: case NOP_BIND_SSBO: {
            const auto* b = reinterpret_cast<const nvn_p_bind_buf*>(a);
            if (b->stage >= 0 && b->stage < kNvnStages && b->index >= 0 && b->index < 16)
                (h.op == NOP_BIND_UBO ? s.st[b->stage].ubo : s.st[b->stage].ssbo)[b->index] = *b;
            break;
        }
        case NOP_BIND_BUFS: {
            const auto* b = reinterpret_cast<const nvn_p_bind_bufs*>(a);
            if (b->stage < 0 || b->stage >= kNvnStages) break;
            for (int i = 0; i < b->n && b->first + i < 16; i++) {
                nvn_p_bind_buf e{b->stage, b->first + i, b->r[i].address, b->r[i].size};
                (b->ssbo ? s.st[b->stage].ssbo : s.st[b->stage].ubo)[b->first + i] = e;
            }
            break;
        }
        case NOP_BIND_TEX: case NOP_BIND_SAMP: case NOP_BIND_IMG: {
            const auto* b = reinterpret_cast<const nvn_p_bind_handles*>(a);
            if (b->stage < 0 || b->stage >= kNvnStages) break;
            uint64_t* dst = h.op == NOP_BIND_TEX ? s.st[b->stage].tex : h.op == NOP_BIND_SAMP ? s.st[b->stage].samp : s.st[b->stage].img;
            const int cap = h.op == NOP_BIND_IMG ? 32 : 128;
            for (int i = 0; i < b->n && b->first + i < cap; i++) dst[b->first + i] = b->h[i];
            break;
        }
        case NOP_STENCIL: {
            const auto* v = reinterpret_cast<const nvn_p_stencil*>(a);
            for (int f = 0; f < 2; f++) {
                if (!(v->face & (1 << f))) continue;
                if (v->which == 0) s.stencil_value_mask[f] = v->value_mask;
                else if (v->which == 1) s.stencil_mask[f] = v->mask;
                else s.stencil_ref[f] = v->ref;
            }
            break;
        }
        case NOP_POLY_OFFSET: {
            const auto* v = reinterpret_cast<const nvn_p_poly_offset*>(a);
            s.poly_offset[0] = v->factor, s.poly_offset[1] = v->units, s.poly_offset[2] = v->clamp;
            break;
        }
        case NOP_BLEND_COLOR: memcpy(s.blend_color, a, 16); break;
        case NOP_DEPTH_CLAMP: s.depth_clamp = reinterpret_cast<const nvn_p_flag*>(a)->value != 0; break;
        case NOP_RENDER_ENABLE:
            s.render_enabled = reinterpret_cast<const nvn_p_flag*>(a)->value != 0;
            s.cond_ok = true;
            break;
        case NOP_RENDER_COND: render_cond(*reinterpret_cast<const nvn_p_render_cond*>(a)); break;
        case NOP_RASTER_DISCARD: s.raster_discard = reinterpret_cast<const nvn_p_flag*>(a)->value != 0; break;
        case NOP_PRIMITIVE_RESTART: s.restart = reinterpret_cast<const nvn_p_restart*>(a)->enable != 0; break;
        case NOP_COPY_STRIDE: s.copy_stride = reinterpret_cast<const nvn_p_copy_stride*>(a)->stride; break;
        case NOP_DRAW: {
            const auto* d = reinterpret_cast<const nvn_p_draw*>(a);
            draw(d->prim, false, (uint32_t)d->count, (uint32_t)d->first, 0, (uint32_t)d->base_instance, (uint32_t)d->instances, 0, 0);
            break;
        }
        case NOP_DRAW_IDX: {
            const auto* d = reinterpret_cast<const nvn_p_draw_idx*>(a);
            draw(d->prim, true, (uint32_t)d->count, 0, d->base_vertex, (uint32_t)d->base_instance, (uint32_t)d->instances,
                 d->ibuf, d->itype);
            break;
        }
        case NOP_DISPATCH: {
            const auto* d = reinterpret_cast<const nvn_p_dispatch*>(a);
            dispatch((uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z, 0);
            break;
        }
        case NOP_DISPATCH_INDIRECT: dispatch(0, 0, 0, reinterpret_cast<const nvn_p_dispatch_indirect*>(a)->addr); break;
        case NOP_DRAW_INDIRECT: vk_log_limited("indirect", 4, "vk: indirect draws are not replayed yet"); break;
        case NOP_WAIT_EVENT: {
            const auto* w = reinterpret_cast<const nvn_p_wait_event*>(a);
            uint32_t v = 0;
            nvn_gpu_read(w->gpu, &v, 4);
            vk_log_limited("waitevent", 8, "vk: queue waits for event %#llx (mode %u value %u), now %u", (unsigned long long)w->gpu,
                           w->mode, w->value, v);
            break;
        }
        case NOP_BARRIER: end_rendering(); full_barrier(rp.cb); break;
        case NOP_COPY_B2T: copy_buffer_texture(*reinterpret_cast<const nvn_p_copy_bt*>(a), true); break;
        case NOP_COPY_T2B: copy_buffer_texture(*reinterpret_cast<const nvn_p_copy_bt*>(a), false); break;
        case NOP_COPY_T2T: copy_texture_texture(*reinterpret_cast<const nvn_p_copy_tt*>(a)); break;
        case NOP_CLEAR_TEX: clear_texture(*reinterpret_cast<const nvn_p_clear_tex*>(a)); break;
        default: break;
        }
        p += h.size;
    }
}

uint64_t pushed = 0;

}  // namespace

/// HOS_VK_LOG=<n>[@<seconds>] (debugging): the first n (from that time on) render target changes, what was drawn into each, and the presents
int trace_left() {
    static int64_t from = 0;
    static int left = [] {
        const char* e = getenv("HOS_VK_LOG");
        if (e && strchr(e, '@')) from = (int64_t)(atof(strchr(e, '@') + 1) * 1e9);
        return e ? atoi(e) : 0;
    }();
    if (left <= 0 || hos_now_ns() < from) return 0;
    return left--;
}
}  // namespace ovk

// ---- the entry points ----
using namespace ovk;

extern "C" {
// (the null back end's, host/rx_null.c: what is done when Vulkan is not up)
void rx_null_submit(const uint64_t* handles, int n);
void rx_null_read_texels(const nvn_tex* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, uint8_t* dst,
                         size_t row, size_t img);

static bool up() { return g.dev != VK_NULL_HANDLE; }

void rx_nvn_submit_q(const uint64_t* handles, int n, uint64_t queue) {
    if (!up()) {
        rx_null_submit(handles, n);
        __atomic_add_fetch(&pushed, 1, __ATOMIC_RELEASE);
        return;
    }
    std::lock_guard lk(big_lock);
    StatTimer timer{kStatSubmit};
    auto& st = states[queue];
    if (!st) {
        st = std::make_unique<State>();
        hos_log("vk: queue %#llx submits (%zu queues)", (unsigned long long)queue, states.size());
    }
    if (makers.threads < 0) makers.start();
    if (makers.threads > 0) {   // (the programs ahead, on a copy of the state)
        State ahead = *st;
        rp.s = &ahead, rp.scanning = true;
        for (int i = 0; i < n; i++) run(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(handles[i])));
        rp.scanning = false;
    }
    rp.s = st.get();
    rp.cb = begin_commands();
    for (int i = 0; i < n; i++) run(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(handles[i])));
    end_rendering();
    end_and_wait(rp.cb);
    rp.cb = VK_NULL_HANDLE;
    staging_release();
    ring.reset();
    cpu_ops();
    rp.writes.clear(), rp.writes_unknown = false;
    __atomic_add_fetch(&pushed, 1, __ATOMIC_RELEASE);
}
void rx_nvn_submit(const uint64_t* handles, int n) { rx_nvn_submit_q(handles, n, 0); }
uint64_t rx_nvn_fence(void) { return __atomic_load_n(&pushed, __ATOMIC_ACQUIRE); }
uint64_t rx_nvn_fence_q(uint64_t) { return rx_nvn_fence(); }
uint64_t rx_nvn_pushed(void) { return rx_nvn_fence(); }
uint64_t rx_nvn_replayed(void) { return rx_nvn_fence(); }
int rx_nvn_wait(uint64_t, uint64_t) { return 1; }
void rx_wait_writebacks_reading(const void*, uint64_t) {}
void rx_nvn_finish(void) {}

void rx_nvn_present(const nvn_tex* t, const int crop[4]) {
    if (!up() || !t) return;
    std::lock_guard lk(big_lock);
    pass_dump_frame_end();
    Image* im = image_find(t);
    if (trace_left())
        hos_log("vk trace: present %#llx %ux%u format %#x: %s", (unsigned long long)t->gpu, t->d.width, t->d.height,
                t->d.format, im ? "found" : "no image");
    if (im) {
        uint32_t w, h;
        image_extent(im, 0, &w, &h);
        present_image(image_handle(im), image_format(im), w, h, crop);
    }
    textures_frame_end();
    stats_frame_end();
    static uint64_t frames;
    if (++frames % 600 == 0) hos_log("vk: %llu frames; %llu draws, %llu dispatches, %llu skipped", (unsigned long long)frames,
                                     (unsigned long long)rp.draws, (unsigned long long)rp.dispatches,
                                     (unsigned long long)rp.skipped);
}

void rx_nvn_register_texture(int id, const nvn_tex* t, const nvn_view* v) {
    register_texture(id, t, v);   // (reg_lock: vk_texture.cpp)
}
void rx_nvn_register_sampler(int id, const nvn_samp_par* p) {
    register_sampler(id, p);
}
void rx_nvn_set_texture_pool_size(int) {}
void rx_nvn_texture_released(const nvn_tex* t) {
    texture_released(t);
}
void rx_nvn_texture_created(const nvn_tex*) {}
void rx_nvn_program_released(const nvn_prog*) {}
void rx_nvn_program_created(const nvn_prog*) {}

void rx_nvn_read_texels(const nvn_tex* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, uint8_t* dst,
                        size_t row, size_t img) {
    texels_from_guest(t, level, layer0, r, dst, row, img);
}
void rx_nvn_write_texels(const nvn_tex* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, const uint8_t* src,
                         size_t row, size_t img) {
    texels_to_guest(t, level, layer0, r, src, row, img);
    if (!up()) return;
    std::lock_guard lk(big_lock);
    if (Image* im = image_find(t)) image_dirty(im);
}
void rx_nvn_invalidate(uint64_t, uint64_t) {}
void rx_nvn_flushed(uint64_t gpu, uint64_t size) {
    if (!up() || !size) return;
    std::lock_guard lk(big_lock);
    const int n = images_dirty(gpu, size);
    if (n) vk_log_limited("flushed", 8, "vk: the CPU wrote %#llx + %#llx: %d texture(s) filled again", (unsigned long long)gpu,
                          (unsigned long long)size, n);
}
void rx_nvn_pool_created(uint64_t gpu, uint64_t size) {
    if (up()) pool_created(gpu, size);
}
void rx_nvn_vmap_changing(uint64_t, uint64_t) {}
void rx_nvn_event_signal(uint64_t, uint32_t) {}
}
