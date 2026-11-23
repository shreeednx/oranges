// NVN programs to SPIR-V: each stage of an NVN1 (Switch 1) program read from guest memory, translated by eden's
// Maxwell shader recompiler (third_party/eden), and the stages of a pipeline emitted together.
//
// An NVN1 stage (nvnProgramSetShaders: {data, control}) is a control section in CPU memory (magic 0x98761234, version
// 1) and a code area in GPU memory: a 0x30-byte header (magic 0x12345678), then for a graphics stage the 0x50-byte
// Maxwell program header (SPH) at +0x30 and the instructions from +0x80, with a scheduling word every 32 bytes. That is
// what the Maxwell frontend reads from a GPU's program region, so the stage is translated as it is, the program's
// extent found by following its control flow.
//
// What a program reads of the NVN driver's constant banks (measured on TOTK, NVN 55 like this game, by the MIT-licensed
// reference layer d2r-mobile; layouts as gpu/render/rx_nvn.cpp of TOTKPort / Longshot has them):
//   c0  storage buffer table (16 bytes a slot: address, size) at 0x110 (vertex), 0x510 (fragment), 0x310 (compute);
//       compute: the grid at 0x200, its uniform buffers' table at 0x210; vertex: the point size clamp at 0x610
//   c2  texture handles at 0x20 + 8 slot (32 slots) and 0x168 + 8 slot (128), sampler handles at 0x568 + 8 slot,
//       image handles at 0x120 + 8 slot
//   c1  the program's own constants (branch tables, literals): the code area from control +0x700 to control +0x704
//   c3+ the uniform buffers bound to slots 0, 1, ...
// A texture handle is the texture's descriptor id (bits 0-19) and the sampler's (bits 20-31).
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../nvn/rx_nvn.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/shader_info.h"
#include "shader_recompiler/stage.h"

namespace ovk {

constexpr int kNvnStages = 6;   // vertex, fragment, geometry, tess control, tess evaluation, compute (NVNshaderStage)

/// What a translation asks of the state it is drawn with
struct StageQuery {
    virtual ~StageQuery() = default;
    /// a word of constant bank `index` of NVN stage `stage` (the driver banks c0 and c2, the uniform buffers from c3)
    virtual uint32_t cbuf_word(int stage, uint32_t index, uint32_t offset) = 0;
    /// the texture a handle names (Color2D when none)
    virtual Shader::TextureType texture_type(uint32_t handle) = 0;
    virtual bool texture_integer(uint32_t handle) = 0;
};

/// One stage of a program: its code, copied out of guest memory, and its header
struct StageCode {
    int nvn_stage = -1;   // NVNshaderStage
    Shader::Stage stage{};
    uint64_t data = 0, control = 0;   // (where it was read from)
    uint64_t key = 0;                 // hash of its code
    std::vector<uint64_t> code;       // the code area from its start (its header, the program header, the code)
    std::vector<uint8_t> c1;          // constant bank 1: the program's constants (control section +0x700: start, end)
    uint32_t workgroup[3]{1, 1, 1}, shared_memory = 0, local_memory = 0;   // (compute)
    std::string why;                  // (not translatable: why)
    bool ok = false;
    const Shader::ProgramHeader& sph() const {
        return *reinterpret_cast<const Shader::ProgramHeader*>(reinterpret_cast<const uint8_t*>(code.data()) + 0x30);
    }
};

/// The stage at sd, from a cache of stages by code address (checked against guest memory at each use); null when it
/// cannot be read
std::shared_ptr<const StageCode> stage_code(const nvn_stage_data& sd);

/// A pipeline's stages translated and emitted together (vertex then fragment, or compute alone)
struct Translated {
    struct Stage {
        int nvn_stage = -1;
        Shader::Stage stage{};
        std::vector<uint32_t> spirv;
        Shader::Info info;
    };
    std::vector<Stage> stages;
    std::string why;   // (failed: why)
    bool ok = false;
};

struct PipelineInputs {
    std::array<Shader::AttributeType, 32> attributes{};   // the vertex attributes' types (Disabled: not enabled)
    std::array<Shader::AttributeType, 8> color_outputs{};  // the render targets' kinds
    bool convert_depth = true;                             // NVN's -1..1 depth (NEAR_IS_MINUS_W) to Vulkan's 0..1
};

/// Translates the stages (each a StageCode of a different NVN stage) as one pipeline, asking q about the state
Translated translate(const std::vector<std::shared_ptr<const StageCode>>& stages, const PipelineInputs& in, StageQuery& q);

}  // namespace ovk
