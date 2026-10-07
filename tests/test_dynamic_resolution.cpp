// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include "video_core/renderer_vulkan/dynamic_resolution.h"

int main() {
    using namespace Vulkan::DynamicResolution;
    assert(Seed(960, 1920, 5, 100) == 50);
    assert(Seed(1280, 1920, 5, 100) == 67);
    assert(Seed(3840, 1920, 5, 200) == 200);
    assert(Seed(96, 1920, 25, 100) == 25);
    assert(Seed(960, 0, 5, 100) == 100);
    assert(Step(100, 80, 5, 100) == 99);
    assert(Step(50, 55, 5, 100) == 51);
    assert(Step(29, 35, 5, 100) == 30); // rollback is gradual too
    assert(Step(30, 30, 5, 100) == 30);
    assert(Step(5, 1, 5, 100) == 5);
    assert(Step(99, 105, 5, 100) == 100);
    assert(Step(20, 19, 25, 100) == 25); // provider safety bound
    assert(Step(100, 100, 120, 100) == 100);
    // Sustained load converges rather than alternating between coarse sizes.
    int scale = 70;
    for (int i = 0; i < 20; ++i) {
        const int next = Step(scale, 50, 5, 100);
        assert(next == scale - 1);
        scale = next;
    }
    assert(scale == 50);
    for (int i = 0; i < 20; ++i) scale = Step(scale, 70, 5, 100);
    assert(scale == 70);
    assert(AssessLowering(50, 49, 15.0, 15.0) == LoweringResult::Pending);
    assert(AssessLowering(50, 46, 15.0, 15.0) == LoweringResult::Rollback);
    assert(AssessLowering(50, 46, 15.0, 14.0) == LoweringResult::Useful);
    assert(AssessLowering(100, 96, 1.0, 1.0) == LoweringResult::Pending);
    // Restoring the measured quality floor must not bypass the visual limit.
    int rollback = 46;
    for (int i = 0; i < 4; ++i) {
        const int next = Step(rollback, 50, 5, 100);
        assert(next == rollback + 1);
        rollback = next;
    }
    assert(rollback == 50);
}
