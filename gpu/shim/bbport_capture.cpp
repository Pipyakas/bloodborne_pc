// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: frame capture and present counting for the control channel (bbport_capture.h).
#include "bbport_capture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include <miniz.h>
#include <vk_mem_alloc.h>

#include "../bbgpu.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace {

enum class Layout { Bgra8, Rgba8, Rgb10a2, Bgr10a2, Unsupported };

struct Frame {
    std::vector<u8> pixels; ///< empty: the capture failed
    u32 width = 0, height = 0;
    Layout layout = Layout::Unsupported;
};

std::mutex capture_mutex; ///< one capture at a time
std::mutex mutex;
std::condition_variable done_cv;
std::atomic<u64> wanted{0}; ///< id of the capture waiting for a frame, 0: none
u64 next_id = 0, done_id = 0;
Frame result;
std::atomic<u64> presents{0};

Layout LayoutOf(vk::Format format) {
    switch (format) {
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
        return Layout::Bgra8;
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
        return Layout::Rgba8;
    case vk::Format::eA2B10G10R10UnormPack32:
        return Layout::Rgb10a2;
    case vk::Format::eA2R10G10B10UnormPack32:
        return Layout::Bgr10a2;
    default:
        return Layout::Unsupported;
    }
}

void Finish(u64 id, Frame&& frame) {
    std::scoped_lock lock{mutex};
    result = std::move(frame);
    done_id = id;
    done_cv.notify_all();
}

/// Adds the pixel at `p` to `sum` (red, green, blue in 0..255).
void Accumulate(const u8* p, Layout layout, u32 sum[3]) {
    switch (layout) {
    case Layout::Bgra8:
        sum[0] += p[2], sum[1] += p[1], sum[2] += p[0];
        break;
    case Layout::Rgba8:
        sum[0] += p[0], sum[1] += p[1], sum[2] += p[2];
        break;
    default: {
        u32 v;
        std::memcpy(&v, p, 4);
        const u32 low = (v >> 2) & 0xff, mid = (v >> 12) & 0xff, high = (v >> 22) & 0xff;
        const bool rgb = layout == Layout::Rgb10a2;
        sum[0] += rgb ? low : high, sum[1] += mid, sum[2] += rgb ? high : low;
        break;
    }
    }
}

} // namespace

namespace BbCapture {

void Record(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler, vk::CommandBuffer cmdbuf,
            vk::Image image, vk::Format format, u32 width, u32 height) {
    const u64 id = wanted.exchange(0, std::memory_order_acq_rel);
    if (!id) {
        return;
    }
    const Layout layout = LayoutOf(format);
    if (layout == Layout::Unsupported) {
        std::printf("Capture: display format %s is not supported\n", vk::to_string(format).c_str());
        Finish(id, {});
        return;
    }
    const VkDeviceSize size = VkDeviceSize(width) * height * 4;
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation, &info) !=
        VK_SUCCESS) {
        Finish(id, {});
        return;
    }
    const vk::BufferImageCopy region{
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {width, height, 1},
    };
    cmdbuf.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, buffer, region);
    const vk::MemoryBarrier to_host{
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eHostRead,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {},
                           to_host, {}, {});
    scheduler.DeferPriorityOperation(
        [allocator = instance.GetAllocator(), buffer, allocation, info, size, id, width, height, layout] {
            vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
            Frame frame{.width = width, .height = height, .layout = layout};
            const u8* data = static_cast<const u8*>(info.pMappedData);
            frame.pixels.assign(data, data + size);
            vmaDestroyBuffer(allocator, buffer, allocation);
            Finish(id, std::move(frame));
        });
}

void Presented() {
    presents.fetch_add(1, std::memory_order_release);
}

} // namespace BbCapture

extern "C" uint64_t bbgpu_present_count(void) {
    return presents.load(std::memory_order_acquire);
}

extern "C" int bbgpu_capture_png(const char* path, int max_width, int timeout_ms, int* out_width,
                                 int* out_height) {
    std::scoped_lock one{capture_mutex};
    Frame frame;
    {
        std::unique_lock lock{mutex};
        const u64 id = ++next_id;
        wanted.store(id, std::memory_order_release);
        if (!done_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                              [id] { return done_id == id; })) {
            // A copy recorded meanwhile completes later and is ignored.
            u64 expected = id;
            wanted.compare_exchange_strong(expected, 0);
            return -1;
        }
        frame = std::move(result);
    }
    if (frame.pixels.empty()) {
        return -1;
    }
    // Area average down to the requested width; RGB without alpha (the game's alpha is not
    // meant for display).
    const u32 w = frame.width, h = frame.height;
    const u32 dw = max_width > 0 && u32(max_width) < w ? u32(max_width) : w;
    const u32 dh = std::max<u32>(1, u32((u64(h) * dw + w / 2) / w));
    std::vector<u8> rgb(size_t(dw) * dh * 3);
    for (u32 dy = 0; dy < dh; ++dy) {
        const u32 y0 = u32(u64(dy) * h / dh), y1 = std::max(y0 + 1, u32(u64(dy + 1) * h / dh));
        for (u32 dx = 0; dx < dw; ++dx) {
            const u32 x0 = u32(u64(dx) * w / dw), x1 = std::max(x0 + 1, u32(u64(dx + 1) * w / dw));
            u32 sum[3] = {};
            for (u32 y = y0; y < y1; ++y) {
                const u8* row = frame.pixels.data() + (size_t(y) * w + x0) * 4;
                for (u32 x = x0; x < x1; ++x, row += 4) {
                    Accumulate(row, frame.layout, sum);
                }
            }
            const u32 n = (y1 - y0) * (x1 - x0);
            u8* out = rgb.data() + (size_t(dy) * dw + dx) * 3;
            for (int c = 0; c < 3; ++c) {
                out[c] = u8((sum[c] + n / 2) / n);
            }
        }
    }
    size_t length = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(rgb.data(), int(dw), int(dh), 3, &length,
                                                           MZ_BEST_SPEED, MZ_FALSE);
    if (!png) {
        return -1;
    }
    std::FILE* file = std::fopen(path, "wb");
    const bool written = file && std::fwrite(png, 1, length, file) == length;
    if (file) {
        std::fclose(file);
    }
    mz_free(png);
    if (!written) {
        return -1;
    }
    if (out_width) *out_width = int(dw);
    if (out_height) *out_height = int(dh);
    return 0;
}
