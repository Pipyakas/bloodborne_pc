// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cmath>

namespace Vulkan::DynamicResolution {
// The controller's fitted target is not a safe visual transition. Limit both
// directions (including rollback) to one percentage point per measurement.
// Provider bounds take priority if the previous size is no longer supported.
inline int Step(int current, int requested, int floor, int ceiling) {
    floor = std::min(floor, ceiling);
    return std::clamp(std::clamp(requested, current - 1, current + 1), floor, ceiling);
}

// Enabling DRS should not first jump from a fixed preset to native resolution.
inline int Seed(unsigned render_width, unsigned output_width, int floor, int ceiling) {
    floor = std::min(floor, ceiling);
    const int percent = output_width ? int(std::lround(100.0 * render_width / output_width))
                                     : ceiling;
    return std::clamp(percent, floor, ceiling);
}
} // namespace Vulkan::DynamicResolution
