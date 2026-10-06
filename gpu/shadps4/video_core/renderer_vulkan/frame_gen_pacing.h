// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

namespace Vulkan::FrameGen {
// Do not throttle real frames for generated frames which are only requested, not presented.
// The normal presenter owns the real-frame limit when FG is off, dynamic, or ineffective.
constexpr std::uint32_t RenderedLimitUs(std::uint32_t target_hz,
                                      std::uint32_t requested_multiplier,
                                      std::uint32_t presented_multiplier, bool fixed_generation) {
    if (!fixed_generation || !target_hz || requested_multiplier <= 1 ||
        presented_multiplier <= 1) {
        return 0;
    }
    return std::uint32_t(1000000ull * std::min(requested_multiplier, presented_multiplier) /
                         target_hz);
}
} // namespace Vulkan::FrameGen
