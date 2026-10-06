// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace Vulkan::FrameGen {
// The selected FPS limit belongs to real rendered frames. FG adds frames on top;
// neither the requested nor the confirmed multiplier may divide this limit.
constexpr std::uint32_t RenderedLimitUs(std::uint32_t base_frame_limit) {
    return base_frame_limit ? std::uint32_t(1000000ull / base_frame_limit) : 0;
}
} // namespace Vulkan::FrameGen
