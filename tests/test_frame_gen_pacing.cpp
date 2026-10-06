// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "gpu/shadps4/video_core/renderer_vulkan/frame_gen_pacing.h"

int main() {
    using Vulkan::FrameGen::RenderedLimitUs;
    // Requested 4x must not force 30 FPS when the plugin presents only real frames.
    assert(RenderedLimitUs(120, 4, 0, true) == 0);
    assert(RenderedLimitUs(120, 4, 1, true) == 0);
    assert(RenderedLimitUs(120, 4, 4, true) == 33333);
    assert(RenderedLimitUs(120, 4, 2, true) == 16666);
    assert(RenderedLimitUs(120, 2, 4, true) == 16666);
    assert(RenderedLimitUs(120, 3, 3, true) == 25000);
    // Respect lower selected limits, and no divided cap for off/dynamic/uncapped modes.
    assert(RenderedLimitUs(60, 4, 4, true) == 66666);
    assert(RenderedLimitUs(120, 1, 4, true) == 0);
    assert(RenderedLimitUs(120, 4, 4, false) == 0);
    assert(RenderedLimitUs(0, 4, 4, true) == 0);
    std::puts("Frame generation pacing: PASS (inactive, confirmed multipliers, limits, dynamic)");
}
