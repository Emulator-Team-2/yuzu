// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <vector>
#include <memory>
#include <vulkan/vulkan.hpp>
#include "common/common_types.h"

namespace Vulkan {

class VulkanDevice;
class VulkanFence;
class VulkanResourceManager;

class VulkanScheduler {
public:
    explicit VulkanScheduler(VulkanResourceManager& resource_manager,
                             const VulkanDevice& device_handler);
    ~VulkanScheduler();

    VulkanFence& BeginPass(bool take_fence_ownership = true);

    vk::CommandBuffer BeginRecord();

    void EndRecord(vk::CommandBuffer cmdbuf);

    void EndPass();

    void Flush();

    vk::Semaphore QuerySemaphore();

private:
    struct Call {
        VulkanFence* fence;
        vk::Semaphore semaphore;
        std::vector<vk::CommandBuffer> commands;
        std::vector<vk::CommandBuffer> cmdbufs;
        std::vector<vk::Semaphore> signal_semaphores;
        std::vector<vk::Semaphore> wait_semaphores;
        std::vector<vk::PipelineStageFlags> pipeline_stages;
        std::vector<vk::SubmitInfo> submit_infos;
        bool take_fence_ownership;
    };

    VulkanResourceManager& resource_manager;
    const vk::Device device;
    const vk::Queue queue;

    std::unique_ptr<Call> pass;
    std::vector<std::unique_ptr<Call>> scheduled_passes;
    u32 flush_ticks = 0;

    VulkanFence* next_fence = nullptr;
    vk::Semaphore previous_semaphore = nullptr;

    bool recording_submit = false;
};

} // namespace Vulkan