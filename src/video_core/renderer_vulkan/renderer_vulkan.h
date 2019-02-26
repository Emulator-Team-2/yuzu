// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <optional>
#include <vector>
#include "video_core/renderer_base.h"
#include "video_core/renderer_vulkan/declarations.h"

namespace Core {
class System;
} // namespace Core

namespace Vulkan {

class VKBlitScreen;
class VKDevice;
class VKFence;
class VKMemoryManager;
class VKResourceManager;
class VKSwapchain;
class VKScheduler;
class VKImage;

struct VKScreenInfo {
    u32 width{};
    u32 height{};
    VKImage* image{};
};

class RendererVulkan : public VideoCore::RendererBase {
public:
    explicit RendererVulkan(Core::Frontend::EmuWindow& window, Core::System& system);
    ~RendererVulkan() override;

    /// Swap buffers (render frame)
    void SwapBuffers(
        std::optional<std::reference_wrapper<const Tegra::FramebufferConfig>> framebuffer) override;

    /// Initialize the renderer
    bool Init() override;

    /// Shutdown the renderer
    void ShutDown() override;

private:
    bool PickDevices(const vk::DispatchLoaderDynamic& dldi);

    Core::System& system;

    vk::Instance instance;
    vk::SurfaceKHR surface;

    VKScreenInfo screen_info;

    std::unique_ptr<VKDevice> device;
    std::unique_ptr<VKSwapchain> swapchain;
    std::unique_ptr<VKMemoryManager> memory_manager;
    std::unique_ptr<VKResourceManager> resource_manager;
    std::unique_ptr<VKScheduler> sched;
    std::unique_ptr<VKBlitScreen> blit_screen;
};

} // namespace Vulkan