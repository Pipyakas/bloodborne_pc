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

enum class LoweringResult { Pending, Useful, Rollback };

inline LoweringResult AssessLowering(int from, int current, double before_ms, double after_ms) {
    // A one-point change often predicts less than the timer/scene noise. Keep
    // the baseline across several steps instead of forgetting it every window.
    if (from <= 0 || from - current < 4 || !(before_ms > 0.0) ||
        !std::isfinite(before_ms) || !std::isfinite(after_ms)) return LoweringResult::Pending;
    const double predicted = before_ms *
        (1.0 - double(current * current) / double(from * from));
    if (predicted < 1.0) return LoweringResult::Pending;
    return before_ms - after_ms < 0.1 * predicted ? LoweringResult::Rollback
                                                 : LoweringResult::Useful;
}
} // namespace Vulkan::DynamicResolution
