// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstddef>
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "common/static_vector.h"
#include "core/core.h"
#include "core/memory.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/vk_device.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_renderpass_cache.h"
#include "video_core/renderer_vulkan/vk_resource_manager.h"
#include "video_core/renderer_vulkan/vk_shader_gen.h"

#pragma optimize("", off)

namespace Vulkan {

// How many sets are created per descriptor pool.
static constexpr std::size_t SETS_PER_POOL = 0x400;

/// Gets the address for the specified shader stage program
static VAddr GetShaderAddress(Maxwell::ShaderProgram program) {
    const auto& gpu = Core::System::GetInstance().GPU().Maxwell3D();

    const auto& shader_config = gpu.regs.shader_config[static_cast<std::size_t>(program)];
    return *gpu.memory_manager.GpuToCpuAddress(gpu.regs.code_address.CodeAddress() +
                                               shader_config.offset);
}

static std::size_t GetStageFromProgram(std::size_t program) {
    return program == 0 ? 0 : program - 1;
}

static Maxwell::ShaderStage GetStageFromProgram(Maxwell::ShaderProgram program) {
    return static_cast<Maxwell::ShaderStage>(
        GetStageFromProgram(static_cast<std::size_t>(program)));
}

/// Gets the shader program code from memory for the specified address
static VKShader::ProgramCode GetShaderCode(VAddr addr) {
    VKShader::ProgramCode program_code(VKShader::MAX_PROGRAM_CODE_LENGTH);
    Memory::ReadBlock(addr, program_code.data(), program_code.size() * sizeof(u64));
    return program_code;
}

static vk::StencilOpState GetStencilFaceState(const PipelineParams::StencilFace& state) {
    return vk::StencilOpState(MaxwellToVK::StencilOp(state.op_fail),
        MaxwellToVK::StencilOp(state.op_zpass),
        MaxwellToVK::StencilOp(state.op_zfail),
        MaxwellToVK::ComparisonOp(state.func_func), {}, state.mask
}

class CachedShader::DescriptorPool final : public VKFencedPool {
public:
    explicit DescriptorPool(vk::Device device,
                            const std::vector<vk::DescriptorPoolSize>& pool_sizes,
                            const vk::DescriptorSetLayout layout)
        : pool_ci(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, SETS_PER_POOL,
                  static_cast<u32>(stored_pool_sizes.size()), stored_pool_sizes.data()),
          stored_pool_sizes(pool_sizes), layout(layout), device(device) {

        InitResizable(SETS_PER_POOL, SETS_PER_POOL);
    }

    ~DescriptorPool() = default;

    vk::DescriptorSet Commit(VKFence& fence) {
        const std::size_t index = ResourceCommit(fence);
        const auto pool_index = index / SETS_PER_POOL;
        const auto set_index = index % SETS_PER_POOL;
        return allocations[pool_index][set_index].get();
    }

protected:
    void Allocate(std::size_t begin, std::size_t end) override {
        ASSERT_MSG(begin % SETS_PER_POOL == 0 && end % SETS_PER_POOL == 0, "Not aligned.");

        auto pool = device.createDescriptorPoolUnique(pool_ci);
        std::vector<vk::DescriptorSetLayout> layout_clones(SETS_PER_POOL, layout);

        const vk::DescriptorSetAllocateInfo descriptor_set_ai(*pool, SETS_PER_POOL,
                                                              layout_clones.data());

        pools.push_back(std::move(pool));
        allocations.push_back(device.allocateDescriptorSetsUnique(descriptor_set_ai));
    }

private:
    const vk::Device device;
    const std::vector<vk::DescriptorPoolSize> stored_pool_sizes;
    const vk::DescriptorPoolCreateInfo pool_ci;
    const vk::DescriptorSetLayout layout;

    std::vector<vk::UniqueDescriptorPool> pools;
    std::vector<std::vector<vk::UniqueDescriptorSet>> allocations;
};

CachedShader::CachedShader(VKDevice& device_handler, VAddr addr,
                           Maxwell::ShaderProgram program_type)
    : addr(addr),
      program_type{program_type}, setup{GetShaderCode(addr)}, device{device_handler.GetLogical()} {

    VKShader::ProgramResult program_result = [&]() {
        switch (program_type) {
        case Maxwell::ShaderProgram::VertexA:
            // VertexB is always enabled, so when VertexA is enabled, we have two vertex shaders.
            // Conventional HW does not support this, so we combine VertexA and VertexB into one
            // stage here.
            setup.SetProgramB(GetShaderCode(GetShaderAddress(Maxwell::ShaderProgram::VertexB)));
        case Maxwell::ShaderProgram::VertexB:
            return VKShader::GenerateVertexShader(setup);
        case Maxwell::ShaderProgram::Fragment:
            return VKShader::GenerateFragmentShader(setup);
        default:
            LOG_CRITICAL(HW_GPU, "Unimplemented program_type={}", static_cast<u32>(program_type));
            UNREACHABLE();
        }
    }();

    entries = program_result.entries;

    const vk::ShaderModuleCreateInfo shader_module_ci(
        {}, program_result.code.size(), reinterpret_cast<const u32*>(program_result.code.data()));
    shader_module = device.createShaderModuleUnique(shader_module_ci);

    CreateDescriptorSetLayout();
    CreateDescriptorPool();
}

vk::DescriptorSet CachedShader::CommitDescriptorSet(VKFence& fence) {
    if (descriptor_pool == nullptr) {
        // If the descriptor pool has not been initialized, it means that the shader doesn't used
        // descriptors. Return a null descriptor set.
        return nullptr;
    }
    return descriptor_pool->Commit(fence);
}

void CachedShader::CreateDescriptorSetLayout() {
    const vk::ShaderStageFlags stage = MaxwellToVK::ShaderStage(GetStageFromProgram(program_type));

    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    for (const auto& cbuf_entry : entries.const_buffers) {
        bindings.push_back(
            {cbuf_entry.GetBinding(), vk::DescriptorType::eUniformBuffer, 1, stage, nullptr});
    }
    for (const auto& sampler_entry : entries.samplers) {
        bindings.push_back({sampler_entry.GetBinding(), vk::DescriptorType::eCombinedImageSampler,
                            1, stage, nullptr});
    }

    descriptor_set_layout = device.createDescriptorSetLayoutUnique(
        {{}, static_cast<u32>(bindings.size()), bindings.data()});
}

void CachedShader::CreateDescriptorPool() {
    std::vector<vk::DescriptorPoolSize> pool_sizes;

    const auto PushSize = [&](vk::DescriptorType descriptor_type, std::size_t size) {
        if (size > 0) {
            pool_sizes.push_back({descriptor_type, static_cast<u32>(size) * SETS_PER_POOL});
        }
    };
    PushSize(vk::DescriptorType::eUniformBuffer, entries.const_buffers.size());
    PushSize(vk::DescriptorType::eInputAttachment, entries.attributes.size());
    PushSize(vk::DescriptorType::eCombinedImageSampler, entries.samplers.size());

    if (pool_sizes.size() == 0) {
        // If the shader doesn't use descriptor sets, skip the pool creation.
        return;
    }

    descriptor_pool = std::make_unique<DescriptorPool>(device, pool_sizes, *descriptor_set_layout);
}

VKPipelineCache::VKPipelineCache(RasterizerVulkan& rasterizer, VKDevice& device_handler)
    : RasterizerCache{rasterizer},
      device_handler{device_handler}, device{device_handler.GetLogical()} {

    empty_set_layout = device.createDescriptorSetLayoutUnique({{}, 0, nullptr});
}

Pipeline VKPipelineCache::GetPipeline(const PipelineParams& params,
                                      const RenderPassParams& renderpass_params,
                                      vk::RenderPass renderpass) {
    const auto& gpu = Core::System::GetInstance().GPU().Maxwell3D();

    Pipeline pipeline;
    ShaderPipeline shaders{};

    for (std::size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        const auto& shader_config = gpu.regs.shader_config[index];
        const auto program{static_cast<Maxwell::ShaderProgram>(index)};

        // Skip stages that are not enabled
        if (!gpu.regs.IsShaderConfigEnabled(index)) {
            continue;
        }

        const VAddr program_addr{GetShaderAddress(program)};
        shaders[index] = program_addr;

        // Look up shader in the cache based on address
        Shader shader{TryGet(program_addr)};

        if (!shader) {
            // No shader found - create a new one
            shader = std::make_shared<CachedShader>(device_handler, program_addr, program);
            Register(shader);
        }

        const std::size_t stage = index == 0 ? 0 : index - 1;
        pipeline.shaders[stage] = std::move(shader);

        // When VertexA is enabled, we have dual vertex shaders
        if (program == Maxwell::ShaderProgram::VertexA) {
            // VertexB was combined with VertexA, so we skip the VertexB iteration
            index++;
        }
    }

    const auto [pair, is_cache_miss] = cache.try_emplace({shaders, renderpass_params, params});
    auto& entry = pair->second;

    if (is_cache_miss) {
        entry = std::make_unique<CacheEntry>();

        entry->layout = CreatePipelineLayout(params, pipeline);
        pipeline.layout = *entry->layout;

        entry->pipeline = CreatePipeline(params, pipeline, renderpass);
    }

    pipeline.handle = *entry->pipeline;
    pipeline.layout = *entry->layout;
    return pipeline;
}

void VKPipelineCache::ObjectInvalidated(const Shader& shader) {
    const VAddr invalidated_addr = shader->GetAddr();
    for (auto it = cache.begin(); it != cache.end();) {
        auto& entry = it->first;
        const bool has_addr = [&]() {
            const auto [shaders, renderpass_params, params] = entry;
            for (auto& shader_addr : shaders) {
                if (shader_addr == invalidated_addr) {
                    return true;
                }
            }
            return false;
        }();
        if (has_addr) {
            it = cache.erase(it);
        } else {
            ++it;
        }
    }
}

vk::UniquePipelineLayout VKPipelineCache::CreatePipelineLayout(const PipelineParams& params,
                                                               const Pipeline& pipeline) const {
    std::array<vk::DescriptorSetLayout, Maxwell::MaxShaderStage> set_layouts{};
    for (std::size_t i = 0; i < Maxwell::MaxShaderStage; ++i) {
        const auto& shader = pipeline.shaders[i];
        set_layouts[i] = shader != nullptr ? shader->GetDescriptorSetLayout() : *empty_set_layout;
    }

    return device.createPipelineLayoutUnique(
        {{}, static_cast<u32>(set_layouts.size()), set_layouts.data(), 0, nullptr});
}

vk::UniquePipeline VKPipelineCache::CreatePipeline(const PipelineParams& params,
                                                   const Pipeline& pipeline,
                                                   vk::RenderPass renderpass) const {
    const auto& vertex_input = params.vertex_input;
    const auto& input_assembly = params.input_assembly;
    const auto& ds = params.depth_stencil;
    const auto& viewport_state = params.viewport_state;

    StaticVector<vk::VertexInputBindingDescription, Maxwell::NumVertexArrays> vertex_bindings;
    for (const auto& binding : vertex_input.bindings) {
        ASSERT(binding.divisor == 0);
        vertex_bindings.Push(vk::VertexInputBindingDescription(binding.index, binding.stride));
    }

    StaticVector<vk::VertexInputAttributeDescription, Maxwell::NumVertexArrays> vertex_attributes;
    for (const auto& attribute : vertex_input.attributes) {
        vertex_attributes.Push(vk::VertexInputAttributeDescription(
            attribute.index, attribute.buffer,
            MaxwellToVK::VertexFormat(attribute.type, attribute.size), attribute.offset));
    }

    const vk::PipelineVertexInputStateCreateInfo vertex_input_ci(
        {}, static_cast<u32>(vertex_bindings.Size()), vertex_bindings.Data(),
        static_cast<u32>(vertex_attributes.Size()), vertex_attributes.Data());

    const vk::PrimitiveTopology primitive_topology =
        MaxwellToVK::PrimitiveTopology(input_assembly.topology);
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly_ci(
        {}, primitive_topology, input_assembly.primitive_restart_enable);

    const vk::Viewport viewport(0.f, 0.f, viewport_state.width, viewport_state.height, 0.f, 1.f);
    // TODO(Rodrigo): Read scissor values instead of using viewport
    const vk::Rect2D scissor(
        {0, 0}, {static_cast<u32>(viewport_state.width), static_cast<u32>(viewport_state.height)});
    const vk::PipelineViewportStateCreateInfo viewport_state_ci({}, 1, &viewport, 1, &scissor);

    const vk::PipelineRasterizationStateCreateInfo rasterizer_ci(
        {}, false, false, vk::PolygonMode::eFill, vk::CullModeFlagBits::eNone,
        vk::FrontFace::eCounterClockwise, false, 0.0f, 0.0f, 0.0f, 1.0f);

    const vk::PipelineMultisampleStateCreateInfo multisampling_ci(
        {}, vk::SampleCountFlagBits::e1, false, 0.0f, nullptr, false, false);

    const vk::CompareOp depth_test_compare = ds.depth_test_enable
                                                 ? MaxwellToVK::ComparisonOp(ds.depth_test_function)
                                                 : vk::CompareOp::eAlways;

    const vk::PipelineDepthStencilStateCreateInfo depth_stencil_ci(
        {}, ds.depth_test_enable, ds.depth_write_enable, depth_test_compare, ds.depth_bounds_enable,
        ds.stencil_enable, {}, {}, ds.depth_bounds_min, ds.depth_bounds_max);

    const vk::PipelineColorBlendAttachmentState color_blend_attachment(
        false, vk::BlendFactor::eZero, vk::BlendFactor::eZero, vk::BlendOp::eAdd,
        vk::BlendFactor::eZero, vk::BlendFactor::eZero, vk::BlendOp::eAdd,
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
    const vk::PipelineColorBlendStateCreateInfo color_blending_ci(
        {}, false, vk::LogicOp::eCopy, 1, &color_blend_attachment, {0.f, 0.f, 0.f, 0.f});

    StaticVector<vk::PipelineShaderStageCreateInfo, Maxwell::MaxShaderStage> shader_stages;
    for (std::size_t stage = 0; stage < Maxwell::MaxShaderStage; ++stage) {
        const auto& shader = pipeline.shaders[stage];
        if (shader == nullptr)
            continue;

        shader_stages.Push(vk::PipelineShaderStageCreateInfo(
            {}, MaxwellToVK::ShaderStage(static_cast<Maxwell::ShaderStage>(stage)),
            shader->GetHandle(primitive_topology), "main", nullptr));
    }

    const vk::GraphicsPipelineCreateInfo create_info(
        {}, static_cast<u32>(shader_stages.Size()), shader_stages.Data(), &vertex_input_ci,
        &input_assembly_ci, nullptr, &viewport_state_ci, &rasterizer_ci, &multisampling_ci,
        &depth_stencil_ci, &color_blending_ci, nullptr, pipeline.layout, renderpass, 0);
    return device.createGraphicsPipelineUnique(nullptr, create_info);
}

} // namespace Vulkan