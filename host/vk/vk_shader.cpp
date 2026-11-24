// NVN programs to SPIR-V (vk_shader.h has the formats). (oranges) Written for NVN1 on the model of Longshot's
// gpu/vk/vk_shader.cpp (https://git.magicbean.dev/MagicBeanDev/Longshot, GPL-3.0-or-later): its environment over NVN
// state, profile and translation; the NVN2 package reading, Ampere lowering, mesh stages, seeds and caches are left out.
#include "vk_shader.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/mma_layout.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "vk_log.h"

namespace ovk {
namespace {

constexpr uint32_t kControlMagic = 0x98761234;
constexpr uint32_t kCodeMagic = 0x12345678;
constexpr uint32_t kCodeHeader = 0x30;   // (the code area's own header, before the program header)
constexpr uint32_t kMaxCode = 1u << 20;  // (bytes of code a stage may have)
constexpr uint32_t kControlC1 = 0x700;   // (the control section: the c1 block's start and end in the code area)

// (the control section: the code's start in the code area (graphics 0x30, past the code area's header; compute 0), the
// NVN stage, and a compute stage's workgroup size (x, y, z) and shared memory bytes; found on the game's programs)
constexpr uint32_t kControlStart = 0x708, kControlStage = 0x714, kControlWorkgroup = 0x748;

/// the first instruction's offset from the program's start (graphics: past the program header; eden's CFG skips a
/// scheduling word there itself)
constexpr uint32_t first_instruction(bool graphics) { return graphics ? 0x50 : 0; }

uint64_t hash_bytes(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

Shader::Stage stage_of(int nvn_stage) {
    switch (nvn_stage) {
    case 0: return Shader::Stage::VertexB;
    case 1: return Shader::Stage::Fragment;
    case 2: return Shader::Stage::Geometry;
    case 3: return Shader::Stage::TessellationControl;
    case 4: return Shader::Stage::TessellationEval;
    default: return Shader::Stage::Compute;
    }
}
int nvn_stage_of_header(uint32_t shader_type) {   // (the SPH's shader type: 1 vertex 2 tess control 3 tess eval 4 geometry 5 pixel)
    switch (shader_type) {
    case 1: return 0;
    case 2: return 3;
    case 3: return 4;
    case 4: return 2;
    case 5: return 1;
    default: return -1;
    }
}

/// Reads a stage's code to the end of what its control flow reaches: the frontend's CFG over guest memory, the
/// largest address it read is the extent
class ExtentEnv final : public Shader::Environment {
public:
    ExtentEnv(uint64_t base, Shader::Stage st, uint32_t start, const std::vector<uint8_t>& c1) : base_{base}, c1_{c1} {
        stage = st;
        start_address = start;
    }
    u64 ReadInstruction(u32 address) override {
        uint64_t v = 0;
        if (address < kMaxCode && nvn_gpu_read(base_ + address, &v, 8)) end_ = std::max<uint32_t>(end_, address + 8);
        return v;
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {   // (c1: the branch tables the control flow follows)
        uint32_t v = 0;
        if (index == 1 && (size_t)offset + 4 <= c1_.size()) std::memcpy(&v, c1_.data() + offset, 4);
        return v;
    }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 2; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return std::nullopt; }
    void Dump(u64, u64) override {}
    uint32_t end() const { return end_; }

private:
    uint64_t base_;
    const std::vector<uint8_t>& c1_;
    uint32_t end_ = 0;
};

std::shared_ptr<StageCode> read_stage(const nvn_stage_data& sd) {
    auto c = std::make_shared<StageCode>();
    c->data = sd.data, c->control = sd.control;
    uint32_t magic = 0;
    if (!sd.control || !sd.data || !nvn_gpu_read(sd.control, &magic, 4) || magic != kControlMagic) {
        c->why = "not an NVN1 shader (control magic)";
        return c;
    }
    uint32_t start = 0, stage = 0;
    nvn_gpu_read(sd.control + kControlStart, &start, 4);
    nvn_gpu_read(sd.control + kControlStage, &stage, 4);
    if (stage == 5) {   // compute: the code from the start of the code area; its workgroup and shared memory
        uint32_t cs[4] = {1, 1, 1, 0};
        nvn_gpu_read(sd.control + kControlWorkgroup, cs, sizeof cs);
        for (int i = 0; i < 3; i++) c->workgroup[i] = std::max(1u, cs[i]);
        c->shared_memory = cs[3];
        c->start = start;
    } else {   // graphics: the code area's header and the program header first
        uint32_t code_magic = 0, word0 = 0;
        if (!nvn_gpu_read(sd.data, &code_magic, 4) || code_magic != kCodeMagic) {
            c->why = "not an NVN1 shader (code magic)";
            return c;
        }
        nvn_gpu_read(sd.data + kCodeHeader, &word0, 4);
        const uint32_t sph_type = word0 & 31, shader_type = word0 >> 10 & 15;
        if (!((sph_type == 1 || sph_type == 2) && (word0 >> 5 & 31) == 3 && nvn_stage_of_header(shader_type) >= 0)) {
            c->why = "stage " + std::to_string(stage) + " without a program header";
            return c;
        }
        stage = (uint32_t)nvn_stage_of_header(shader_type);
        c->start = kCodeHeader;
    }
    const bool graphics = stage != 5;
    c->nvn_stage = (int)stage;
    c->stage = stage_of(c->nvn_stage);
    // c1: the program's own constants (indirect branch tables, literals), after its code in the code area
    uint32_t c1[2] = {0, 0};
    if (nvn_gpu_read(sd.control + kControlC1, c1, 8) && c1[0] <= c1[1] && c1[1] - c1[0] <= 0x10000) {
        c->c1.resize(c1[1] - c1[0]);
        if (!c->c1.empty() && !nvn_gpu_read(sd.data + c1[0], c->c1.data(), c->c1.size())) c->c1.clear();
    }
    // the extent: what the control flow reaches. Addresses are the code area's: its scheduling words are at its 32-byte
    // boundaries (+0x80 is one), so the program starts at +0x30 there, and its first instruction is past the header.
    uint32_t end = 0;
    try {
        ExtentEnv env{sd.data, c->stage, c->start, c->c1};
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> blocks{32};
        Shader::Maxwell::Flow::CFG cfg{env, blocks, c->start + first_instruction(graphics)};
        end = env.end();
    } catch (const std::exception& e) {
        c->why = std::string("control flow: ") + e.what();
        return c;
    }
    if (end <= c->start + first_instruction(graphics)) {
        c->why = "no code";
        return c;
    }
    end = (end + 31) & ~31u;
    c->code.assign(end / 8, 0);
    if (!nvn_gpu_read(sd.data, c->code.data(), end)) return c->why = "code not readable", c;
    c->key = hash_bytes(c->c1.data(), c->c1.size(), hash_bytes(c->code.data(), end, (uint64_t)c->nvn_stage));
    c->key = hash_bytes(c->workgroup, sizeof c->workgroup, hash_bytes(&c->shared_memory, 4, c->key));
    if (graphics) {
        const auto& h = c->sph();
        c->local_memory = static_cast<uint32_t>(h.LocalMemorySize()) + h.common3.shader_local_memory_crs_size;
    }
    c->ok = true;
    return c;
}

}  // namespace

std::shared_ptr<const StageCode> stage_code(const nvn_stage_data& sd) {
    static std::mutex mu;
    static std::unordered_map<uint64_t, std::shared_ptr<StageCode>> by_data;
    {
        std::lock_guard lk(mu);
        const auto it = by_data.find(sd.data ^ sd.control << 1);
        if (it != by_data.end()) {
            const StageCode& c = *it->second;
            if (!c.ok) return it->second;
            // (still the same code there: compared, not hashed; the game may load other programs at an address)
            const size_t n = c.code.size() * 8;
            const uint8_t* now = nvn_gpu_host(sd.data);
            if (now && nvn_gpu_contiguous(sd.data, n) >= n && !std::memcmp(now, c.code.data(), n)) return it->second;
        }
    }
    auto c = read_stage(sd);
    if (!c->ok) vk_log_limited("stage", 32, "vk: program stage at %#llx (control %#llx): %s", (unsigned long long)sd.data,
                               (unsigned long long)sd.control, c->why.c_str());
    std::lock_guard lk(mu);
    by_data[sd.data ^ sd.control << 1] = c;
    return c;
}

namespace {

/// eden's environment over an NVN stage and the state it is drawn with
class Env final : public Shader::Environment {
public:
    Env(const StageCode& c, StageQuery& q) : c_{c}, q_{q} {
        stage = c.stage;
        start_address = c.start;
        if (c.nvn_stage != 5) std::memcpy(&sph, reinterpret_cast<const uint8_t*>(c.code.data()) + kCodeHeader, sizeof sph);
        is_proprietary_driver = true;
    }
    u64 ReadInstruction(u32 address) override { return address / 8 < c_.code.size() ? c_.code[address / 8] : 0; }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        if (index == 1) {   // (the program's own: folded into the translation, as on NVIDIA's driver)
            uint32_t v = 0;
            if ((size_t)offset + 4 <= c_.c1.size()) std::memcpy(&v, c_.c1.data() + offset, 4);
            return v;
        }
        return q_.cbuf_word(c_.nvn_stage, index, offset);
    }
    Shader::TextureType ReadTextureType(u32 handle) override { return q_.texture_type(handle); }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32 handle) override { return q_.texture_integer(handle); }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 2; }
    u32 LocalMemorySize() const override { return c_.local_memory; }
    u32 SharedMemorySize() const override { return c_.shared_memory; }
    std::array<u32, 3> WorkgroupSize() const override {
        return {std::max(1u, c_.workgroup[0]), std::max(1u, c_.workgroup[1]), std::max(1u, c_.workgroup[2])};
    }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return std::nullopt; }
    void Dump(u64, u64) override {}

private:
    const StageCode& c_;
    StageQuery& q_;
};

Shader::Profile make_profile() {
    Shader::Profile p{};
    p.supported_spirv = 0x00010600;
    p.unified_descriptor_binding = true;
    p.support_descriptor_aliasing = true;
    p.support_int8 = p.support_int16 = p.support_int64 = true;
    p.support_uniform_and_storage_buffer_8bit = p.support_storage_buffer_8bit = true;
    p.support_uniform_and_storage_buffer_16bit = p.support_storage_buffer_16bit = true;
    p.support_vertex_instance_id = false;
    p.support_float_controls = true;
    p.support_separate_denorm_behavior = p.support_separate_rounding_mode = true;
    p.support_fp32_denorm_preserve = p.support_fp32_denorm_flush = true;
    p.support_fp16_signed_zero_nan_preserve = p.support_fp32_signed_zero_nan_preserve = true;
    p.support_vote = true;
    p.supported_subgroup_stages = 0x7f;
    p.support_viewport_index_layer_non_geometry = true;
    p.support_typeless_image_loads = true;
    p.support_demote_to_helper_invocation = true;
    p.support_int64_atomics = true;
    p.support_derivative_control = true;
    p.support_native_ndc = false;
    p.support_scaled_attributes = false;
    p.support_multi_viewport = true;
    p.support_sampled_image_array_nonuniform_indexing = true;
    p.warp_size_potentially_larger_than_guest = false;
    p.lower_left_origin_mode = false;
    p.need_declared_frag_colors = false;
    p.max_subgroup_size = 32;
    p.min_ssbo_alignment = 16;
    return p;
}

Shader::HostTranslateInfo host_info() {
    Shader::HostTranslateInfo h{};
    h.support_float64 = true;
    h.support_float16 = true;
    h.support_int64 = true;
    h.support_snorm_render_buffer = true;
    h.support_viewport_index_layer = true;
    h.min_ssbo_alignment = 16;
    h.support_conditional_barrier = true;
    h.ApplyDescriptorLimitPolicy();
    return h;
}

struct Pools {
    Shader::ObjectPool<Shader::IR::Inst> insts{8192};
    Shader::ObjectPool<Shader::IR::Block> blocks{32};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_blocks{32};
};

}  // namespace

Translated translate(const std::vector<std::shared_ptr<const StageCode>>& stages, const PipelineInputs& in, StageQuery& q) {
    Translated out;
    static const Shader::Profile profile = make_profile();
    // gl_Position invariant: the game draws a depth pre-pass and then its colour pass with depth EQUAL, which needs both
    // programs' positions bit for bit the same, as the console's compiler makes them; and float colour outputs without
    // NaN or infinity (one NaN texel spreads through the blur and bloom chains). Longshot's switches (mma_layout.h).
    // (set once: translations run on several threads)
    static const bool switches = [] {
        Shader::Backend::SPIRV::position_invariant = true;
        Shader::Backend::SPIRV::frag_nan_guard = true;
        return true;
    }();
    (void)switches;
    static const Shader::HostTranslateInfo host = host_info();
    std::vector<std::unique_ptr<Pools>> pools;
    std::vector<Shader::IR::Program> programs;
    try {
        for (const auto& c : stages) {
            if (!c || !c->ok) {
                out.why = c ? c->why : "no stage";
                return out;
            }
            Env env{*c, q};
            auto& p = pools.emplace_back(std::make_unique<Pools>());
            Shader::Maxwell::Flow::CFG cfg{env, p->flow_blocks, c->start + first_instruction(c->nvn_stage != 5)};
            programs.push_back(Shader::Maxwell::TranslateProgram(p->insts, p->blocks, env, cfg, host));
        }
        Shader::Backend::Bindings bindings{};
        const Shader::IR::Program* previous = nullptr;
        for (size_t i = 0; i < programs.size(); i++) {
            Shader::RuntimeInfo ri{};
            Shader::IR::Program& prog = programs[i];
            if (prog.stage == Shader::Stage::VertexB) ri.generic_input_types = in.attributes;
            if (prog.stage != Shader::Stage::Fragment && prog.stage != Shader::Stage::Compute) ri.convert_depth_mode = in.convert_depth;
            if (previous) {
                ri.previous_stage_stores = previous->info.stores;
                ri.previous_stage_legacy_stores_mapping = previous->info.legacy_stores_mapping;
            } else {   // (the first stage: every input is there to read; eden declares only inputs a stage before stores)
                ri.previous_stage_stores.mask.set();
            }
            if (prog.stage == Shader::Stage::Fragment) ri.color_output_types = in.color_outputs;
            ri.input_topology = Shader::InputTopology::Triangles;
            Translated::Stage s;
            s.nvn_stage = stages[i]->nvn_stage;
            s.stage = prog.stage;
            s.spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, ri, prog, bindings);
            s.info = prog.info;
            out.stages.push_back(std::move(s));
            previous = &prog;
        }
    } catch (const std::exception& e) {
        out.why = e.what();
        out.stages.clear();
        return out;
    }
    out.ok = true;
    return out;
}

}  // namespace ovk
