// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <limits>
#include <optional>
#include <vulkan/vulkan.hpp>
#include "common/common_types.h"

namespace Vulkan {

constexpr auto UNDEFINED_SIZE = std::numeric_limits<u32>::max();
constexpr auto UNDEFINED_FAMILY = std::numeric_limits<u32>::max();

constexpr auto WAIT_UNLIMITED = std::numeric_limits<u64>::max();

} // namespace Vulkan