// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstddef>
#include <memory>
#include <tuple>
#include <vulkan/vulkan.hpp>
#include "common/common_types.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_cache.h"
#include "video_core/renderer_vulkan/vk_stream_buffer.h"

namespace Vulkan {

class RasterizerVulkan;
class VKResourceManager;
class VKDevice;
class VKMemoryManager;
class VKFence;
class VKScheduler;

struct CachedBufferEntry final : public RasterizerCacheObject {
    VAddr GetAddr() const override {
        return addr;
    }

    std::size_t GetSizeInBytes() const override {
        return size;
    }

    // We do not have to flush this cache as things in it are never modified by us.
    void Flush() override {}

    VAddr addr;
    std::size_t size;
    u64 offset;
    vk::Buffer buffer;
    std::size_t alignment;
};

class VKBufferCache final : public RasterizerCache<std::shared_ptr<CachedBufferEntry>> {
public:
    explicit VKBufferCache(RasterizerVulkan& rasterizer, VKResourceManager& resource_manager,
                           VKDevice& device_handler, VKMemoryManager& memory_manager,
                           VKScheduler& sched, u64 size);
    ~VKBufferCache();

    /// Uploads data from a guest GPU address. Returns host's buffer offset where it's been
    /// allocated.
    std::tuple<u64, vk::Buffer> UploadMemory(Tegra::GPUVAddr gpu_addr, std::size_t size,
                                             u64 alignment = 4, bool cache = true);

    /// Uploads from a host memory. Returns host's buffer offset where it's been allocated.
    std::tuple<u64, vk::Buffer> UploadHostMemory(const u8* raw_pointer, std::size_t size,
                                                 u64 alignment = 4);

    /// Reserves memory to be used by host's CPU. Returns mapped address and offset.
    std::tuple<u8*, u64, vk::Buffer> ReserveMemory(std::size_t size, u64 alignment = 4);

    void Reserve(std::size_t max_size);
    void Send(VKFence& fence, vk::CommandBuffer cmdbuf);

protected:
    void AlignBuffer(std::size_t alignment);

private:
    std::unique_ptr<VKStreamBuffer> stream_buffer;

    u8* buffer_ptr = nullptr;
    u64 buffer_offset = 0;
    u64 buffer_offset_base = 0;
    vk::Buffer buffer_handle = nullptr;
};

} // namespace Vulkan