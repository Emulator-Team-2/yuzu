// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include <sirit/sirit.h>

#include "common/common_types.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/shader/shader_ir.h"

namespace VideoCommon::Shader {
class ShaderIR;
}

namespace Vulkan::VKShader {

using Maxwell = Tegra::Engines::Maxwell3D::Regs;

class ConstBufferEntry : public VideoCommon::Shader::ConstBuffer {
public:
    explicit constexpr ConstBufferEntry(const VideoCommon::Shader::ConstBuffer& entry, u32 index)
        : VideoCommon::Shader::ConstBuffer{entry}, index{index} {}

    constexpr u32 GetIndex() const {
        return index;
    }

private:
    u32 index{};
};

using SamplerEntry = VideoCommon::Shader::Sampler;

struct ShaderEntries {
    u32 constant_buffers_base_binding;
    u32 samplers_base_binding;
    std::vector<ConstBufferEntry> const_buffers;
    std::vector<SamplerEntry> samplers;
    std::set<u32> attributes;
    std::array<bool, Maxwell::NumClipDistances> clip_distances{};
    std::size_t shader_length{};
    Sirit::Id entry_function{};
    std::vector<Sirit::Id> interfaces;
};

using DecompilerResult = std::pair<std::unique_ptr<Sirit::Module>, ShaderEntries>;

DecompilerResult Decompile(const VideoCommon::Shader::ShaderIR& ir, Maxwell::ShaderStage stage);

} // namespace Vulkan::VKShader