// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: frames for the control channel (src/runtime_control.c): the presenter copies the
// game frame of the next present to host memory when a capture is wanted, and counts presents.
// The C side (bbgpu_capture_png, bbgpu_present_count) waits for them.

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace BbCapture {

/// Present thread, after the frame was blitted: `image` (layout TransferSrcOptimal) is copied
/// into a host buffer when a capture waits; the copy completes once `scheduler`'s work does.
void Record(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler, vk::CommandBuffer cmdbuf,
            vk::Image image, vk::Format format, u32 width, u32 height);

/// Present thread, after every present.
void Presented();

} // namespace BbCapture
