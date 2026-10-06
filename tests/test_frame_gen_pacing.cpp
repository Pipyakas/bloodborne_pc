// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "gpu/shadps4/video_core/renderer_vulkan/frame_gen_pacing.h"

int main() {
    using Vulkan::FrameGen::RenderedLimitUs;
    // The helper deliberately accepts no FG multiplier: 4x must never turn 40 into 10
    // or 120 into 30. The same base cap applies with FG off, dynamic, 2x, 3x or 4x.
    assert(RenderedLimitUs(40) == 25000);
    assert(RenderedLimitUs(120) == 8333);
    assert(RenderedLimitUs(60) == 16666);
    assert(RenderedLimitUs(30) == 33333);
    assert(RenderedLimitUs(0) == 0);
    std::puts("Frame generation pacing: PASS (base-frame limits, no multiplier division)");
}
