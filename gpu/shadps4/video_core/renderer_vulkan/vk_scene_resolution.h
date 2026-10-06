// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <memory>
#include <optional>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include "video_core/renderer_vulkan/scene_resolution.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore { class TextureCache; }
struct VmaAllocation_T;
struct VmaAllocator_T;

namespace Vulkan {
class Instance;
class Runtime;
class Scheduler;

// Reduced raster targets, with native-size images retained for unmodified guest compute,
// integer texture loads, copies and CPU readbacks. All native access passes Runtime::Transit.
class SceneTargets {
public:
    SceneTargets(const Instance&, Scheduler&, Runtime&, VideoCore::TextureCache&);
    using Lookup = std::function<VideoCore::Image*(VideoCore::ImageId, u64)>;
    SceneTargets(const Instance&, Scheduler&, Runtime&, Lookup);
    ~SceneTargets();
    bool SetSize(SceneResolution::Size);
    SceneResolution::Size Size() const { return size; }
    bool Reduced() const { return size != SceneResolution::Size{}; }
    /// A reduced proxy can stand in for the image: a 1920x1080 scene target or, scaled by
    /// the same factor, a half-resolution (960x540) one (toggle SceneHalfRes).
    bool Eligible(const VideoCore::Image&) const;
    /// Eligible at the full scene size (1920x1080): the upscaler's inputs, the scene start.
    bool EligibleScene(const VideoCore::Image&) const;
    /// The proxy size of an eligible image: the scene size divided like the native size.
    SceneResolution::Size ProxySize(const VideoCore::Image&, u32 level = 0) const;
    /// The largest scene size proxies may take (the preset's, or 100% with dynamic resolution):
    /// their memory is allocated for it once.
    void SetMaxSize(SceneResolution::Size s) {
        max_size = s;
    }
    struct Target {
        vk::Image image;
        vk::ImageView view;
        vk::ImageLayout layout;
        vk::ImageUsageFlags usage;
    };
    Target Attachment(VideoCore::ImageId, const VideoCore::ImageViewInfo&);
    /// `ngx`: DLSS reads the proxy, so it is parked instead of destroyed on resize (`parked`).
    Target Read(VideoCore::ImageId, const VideoCore::ImageViewInfo&,
                vk::PipelineStageFlags2 = vk::PipelineStageFlagBits2::eComputeShader,
                vk::AccessFlags2 = vk::AccessFlagBits2::eShaderRead, bool ngx = false);
    /// Resolve/invalidate only the accessed mip levels; no range means the whole image.
    void NativeAccess(VideoCore::Image&, vk::AccessFlags2,
                      std::optional<VideoCore::SubresourceRange> = {});
    /// The proxy of `image` for sampling when it holds the current content (else nullopt: the
    /// native image is current). Transitions only after other use (attachment, copy).
    /// Mipmapped targets have one proxy per level: views of one level only.
    std::optional<Target> SampleProxy(const VideoCore::Image& image,
                                      const VideoCore::ImageViewInfo& info);
    /// Whether SampleProxy would return the proxy of `image` for these levels.
    [[nodiscard]] bool ProxyCurrent(const VideoCore::Image& image, u32 level = 0,
                                    u32 levels = 1) const;
    /// SampleProxy for a view obtained before (valid while Generation() is unchanged): the
    /// layout to sample it in.
    vk::ImageLayout PrepareSample(const VideoCore::Image& image, u32 level = 0);
    /// Changes when proxies and their views are destroyed (SetSize).
    [[nodiscard]] u64 Generation() const noexcept {
        return generation;
    }
    /// Whether NativeAccess has work for this image (a reduced-size proxy exists).
    [[nodiscard]] bool Tracks(u64 image_uid) const {
        return !copying && tracked.contains(image_uid);
    }
    void ResolveAll();
    /// bbport: copies the reduced proxy of `src` into one for `dst` (same native size and
    /// texel size, single level) when the proxy holds src's newest content: a guest copy of a
    /// scene target then stays reduced (no resolve to the native size, no fill back).
    bool CopyProxy(VideoCore::ImageId src, VideoCore::ImageId dst);
    /// bbport: clears the reduced proxy of a color scene target instead of the native image
    /// (a guest compute clear): no resolve before the clear, no fill after it.
    bool ClearProxy(VideoCore::ImageId id, const vk::ClearColorValue& value,
                    const VideoCore::SubresourceRange& range);
    bool debug = false; ///< BB_SCENE_DEBUG frame: print resolves and fills
private:
    /// A proxy's memory. Sized with headroom over the render size, so dynamic resolution
    /// steps (5-10%) create their images on it instead of allocating; freed when no image
    /// uses it and its proxy was not used at the last size.
    struct Backing {
        VmaAllocator_T* allocator;
        VmaAllocation_T* allocation;
        vk::DeviceSize size;
        u32 memory_type;
        ~Backing();
    };
    struct Entry {
        VideoCore::ImageId source{};
        u64 uid = 0;
        u32 level = 0; ///< mip level of the native image this proxy stands for
        std::shared_ptr<Backing> memory; ///< outlives the image (declared before it)
        VideoCore::UniqueImage image;
        std::vector<std::pair<VideoCore::ImageViewInfo, vk::UniqueImageView>> views;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
        SceneResolution::Coherence state;
        bool kept = false; ///< read by DLSS: parked, not destroyed, on resize
    };
    static constexpr u64 Key(u64 uid, u32 level) {
        return uid << 4 | level;
    }
    /// The proxy of an image level, created on first use and filled from the native image
    /// unless `fill` is false (the caller overwrites it).
    Entry& Get(VideoCore::ImageId, u32 level = 0, bool fill = true);
    vk::ImageView View(Entry&, const VideoCore::Image&, const VideoCore::ImageViewInfo&);
    void Copy(Entry&, VideoCore::Image&, bool to_native);
    // Depth/stencil formats without blit support (D32S8 on RADV) are resampled by a
    // fullscreen draw writing gl_FragDepth and, with stencil export, the stencil value.
    vk::FormatFeatureFlags Features(vk::Format) const;
    bool Blittable(vk::Format) const;
    bool ShaderResampled(const VideoCore::Image&) const;
    void Resample(vk::Image src, vk::Image dst, const VideoCore::Image& original,
                  vk::Extent2D dst_size);
    vk::Pipeline ResamplePipeline(vk::Format, bool stencil);
    void CreateResampleResources();
    void Transition(Entry&, vk::ImageAspectFlags, vk::ImageLayout,
                    vk::PipelineStageFlags2, vk::AccessFlags2);
    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    Lookup lookup;
    SceneResolution::Size size;
    std::unordered_map<u64, std::unique_ptr<Entry>> entries; ///< by Key(uid, level)
    /// Proxies the upscaler read, from earlier sizes. NGX caches image views by handle: a
    /// destroyed view whose handle the driver reuses for a new proxy makes DLSS read through
    /// the stale cache entry (striped, smeared output after live preset or output changes).
    /// They stay alive for the session and are reused when their size returns.
    std::vector<std::unique_ptr<Entry>> parked;
    /// The memory of each proxy (by Key) for the next image at another size.
    std::unordered_map<u64, std::shared_ptr<Backing>> backings;
    SceneResolution::Size max_size{};
    std::unordered_set<u64> tracked; ///< uids with proxies
    bool copying = false;
    bool force_stencil_bits = false; ///< test the portable resampler on any driver
    u64 generation = 0;
    mutable std::unordered_map<vk::Format, vk::FormatFeatureFlags> format_features;
    mutable std::array<vk::FormatFeatureFlags, 256> format_table{};
    mutable std::array<bool, 256> format_known{};
    std::array<std::pair<u64, Entry*>, 8> recent{}; ///< last entries by Key
    u32 recent_next = 0;
    vk::UniqueShaderModule fs_tri_vert, depth_frag, depth_stencil_frag, stencil_bits_frag;
    vk::UniqueDescriptorSetLayout resample_set_layout;
    vk::UniquePipelineLayout resample_layout;
    std::vector<std::pair<std::pair<vk::Format, bool>, vk::UniquePipeline>> resample_pipelines;
};
} // namespace Vulkan
