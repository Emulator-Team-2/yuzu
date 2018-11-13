// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include <memory>
#include <optional>
#include <tuple>
#include "common/alignment.h"
#include "core/core.h"
#include "core/memory.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_stream_buffer.h"

#pragma optimize("", off)

namespace Vulkan {

VulkanBufferCache::VulkanBufferCache(VulkanResourceManager& resource_manager,
                                     VulkanDevice& device_handler,
                                     VulkanMemoryManager& memory_manager, u64 size) {

    stream_buffer = std::make_unique<VulkanStreamBuffer>(
        resource_manager, device_handler, memory_manager, size,
        vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer |
            vk::BufferUsageFlagBits::eUniformBuffer);
}

VulkanBufferCache::~VulkanBufferCache() = default;

std::tuple<u64, vk::Buffer> VulkanBufferCache::UploadMemory(Tegra::GPUVAddr gpu_addr,
                                                            std::size_t size, u64 alignment) {
    auto& emu_memory_manager = Core::System::GetInstance().GPU().MemoryManager();
    const auto cpu_addr{emu_memory_manager.GpuToCpuAddress(gpu_addr)};

    AlignBuffer(alignment);
    const u64 uploaded_offset = buffer_offset;

    Memory::ReadBlock(*cpu_addr, buffer_ptr, size);

    buffer_ptr += size;
    buffer_offset += size;

    return {uploaded_offset, buffer_handle};
}

std::tuple<u64, vk::Buffer> VulkanBufferCache::UploadHostMemory(const u8* raw_pointer,
                                                                std::size_t size, u64 alignment) {
    AlignBuffer(alignment);
    std::memcpy(buffer_ptr, raw_pointer, size);
    const u64 uploaded_offset = buffer_offset;

    buffer_ptr += size;
    buffer_offset += size;
    return {uploaded_offset, buffer_handle};
}

std::tuple<u8*, u64, vk::Buffer> VulkanBufferCache::ReserveMemory(std::size_t size, u64 alignment) {
    AlignBuffer(alignment);
    u8* const uploaded_ptr = buffer_ptr;
    const u64 uploaded_offset = buffer_offset;

    buffer_ptr += size;
    buffer_offset += size;
    return {uploaded_ptr, uploaded_offset, buffer_handle};
}

void VulkanBufferCache::Reserve(std::size_t max_size) {
    bool invalidate;
    std::tie(buffer_ptr, buffer_offset_base, buffer_handle, invalidate) =
        stream_buffer->Reserve(max_size, false);
    buffer_offset = buffer_offset_base;

    if (invalidate) {
        // InvalidateAll();
    }
}

void VulkanBufferCache::Send(VulkanSync& sync, VulkanFence& fence) {
    stream_buffer->Send(sync, fence, buffer_offset - buffer_offset_base);
}

void VulkanBufferCache::AlignBuffer(std::size_t alignment) {
    // Align the offset, not the mapped pointer
    const u64 offset_aligned = Common::AlignUp(buffer_offset, alignment);
    buffer_ptr += offset_aligned - buffer_offset;
    buffer_offset = offset_aligned;
}

} // namespace Vulkan