// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_fsr4.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "ffx_vk_fsr4_v07.h"
#include "ffx_vk_fsr4_v07_assets.h"
#include "bbport_settings.h"
#include "video_core/renderer_vulkan/fsr411/fsr411.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {

std::string AssetDir() {
    const char* dir = std::getenv("BB_FSR4_DIR");
    return dir && dir[0] ? dir : "fsr4_shaders";
}

/// The DLL model's assets (tools/fsr4cap/build_assets.sh): BB_FSR4_DLL_DIR, else fsr4_dll; the
/// names before fsr4_model (BB_FSR411_DIR, fsr4_411) still work.
std::string DllAssetDir() {
    for (const char* name : {"BB_FSR4_DLL_DIR", "BB_FSR411_DIR"}) {
        if (const char* dir = std::getenv(name); dir && dir[0]) {
            return dir;
        }
    }
    std::error_code ec;
    return std::filesystem::is_directory("fsr4_411", ec) && !std::filesystem::is_directory("fsr4_dll", ec)
               ? "fsr4_411"
               : "fsr4_dll";
}

// Outlive the providers: the menus read them through BbSettings::fsr4_{dll,sdk}_version.
std::string dll_version, sdk_version;

/// "upscaler_version" of an asset folder's manifest.json (tools/fsr4cap/manifest.py,
/// tools/fetch_fsr4_assets.sh); empty without one.
std::string ManifestVersion(const std::string& dir) {
    std::ifstream file(dir + "/manifest.json");
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    size_t at = text.find("\"upscaler_version\"");
    if (at == std::string::npos || (at = text.find('"', text.find(':', at))) == std::string::npos) {
        return {};
    }
    const size_t end = text.find('"', at + 1);
    return end == std::string::npos ? std::string{} : text.substr(at + 1, end - at - 1);
}

bool ReadFile(const std::string& path, std::vector<u8>& data) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    data.resize(size_t(file.tellg()));
    file.seekg(0);
    return bool(file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size())));
}

FfxFsr4ModelPreset ModelPreset(int preset) {
    switch (preset) {
    case 0:
        return FFX_FSR4_MODEL_PRESET_NATIVE_AA;
    case 1:
        return FFX_FSR4_MODEL_PRESET_QUALITY;
    case 2:
        return FFX_FSR4_MODEL_PRESET_BALANCED;
    case 3:
        return FFX_FSR4_MODEL_PRESET_PERFORMANCE;
    case 5:
        return FFX_FSR4_MODEL_PRESET_DRS;
    default:
        return FFX_FSR4_MODEL_PRESET_ULTRA_PERFORMANCE;
    }
}

} // namespace

struct Fsr4Upscaler::Impl {
    const Instance& instance;
    Scheduler& scheduler;
    std::vector<u8> scratch;
    FfxInterface backend{};
    bool backend_ok = false;
    ffxContext context{};
    bool context_ok = false;
    int model = -1;
    u32 out_width = 0, out_height = 0;
    std::string problem;
    bool fatal = false;
    u64 next_frame_id = 1;
    std::deque<std::pair<u64, u64>> in_flight; ///< provider frame id, scheduler tick
    // bbport: the DLL model (fsr4_model auto/dll): the replay of the user's AMD upscaler DLL
    // (fsr411/). When it cannot run, v07 runs instead; it is retried for another output size.
    std::unique_ptr<Fsr411::Upscaler> fsr411;
    std::deque<u64> fsr411_ticks; ///< scheduler ticks of its recent frames (constant ring)
    std::string fsr411_described;
    u32 dll_failed_width = 0, dll_failed_height = 0;
    std::string dll_note; ///< why the selected DLL model is not running (menu)

    Impl(const Instance& instance_, Scheduler& scheduler_)
        : instance{instance_}, scheduler{scheduler_} {}

    ~Impl() {
        Destroy();
    }

    void Destroy() {
        if (!backend_ok && !context_ok) {
            return;
        }
        scheduler.Finish();
        if (context_ok) {
            ffxFsr4V07DestroyContext(&context, nullptr);
            context_ok = false;
        }
        if (backend_ok) {
            ffxFsr4VkDestroyContext(reinterpret_cast<FfxFsr4VkContext*>(scratch.data()));
            backend_ok = false;
        }
        backend = {};
        in_flight.clear();
        model = -1;
    }

    void Fail(std::string reason, bool permanent) {
        if (problem != reason) {
            std::printf("Upscaler: FSR 4 unavailable: %s\n", reason.c_str());
        }
        problem = std::move(reason);
        fatal |= permanent;
    }

    bool CreateBackend(int preset, u32 ow, u32 oh) {
        FfxFsr4V07AssetSet assets{};
        if (!ffxFsr4V07BuildAssetSet(ModelPreset(preset), ow, oh, &assets)) {
            Fail("output size is not supported by the v07 model", true);
            return false;
        }
        const std::string dir = AssetDir() + "/";
        std::array<std::vector<u8>, FFX_FSR4_VK_PASS_COUNT> code;
        std::vector<u8> initializer, weights;
        // bbport: tools/fsr4_optimize.sh puts fixed or faster passes into opt/ (post through
        // shared memory, pass 11 without out-of-bounds writes); BB_FSR4_OPT=0 keeps the originals.
        const char* opt_env = std::getenv("BB_FSR4_OPT");
        const bool use_opt = !(opt_env && opt_env[0] == '0');
        u32 optimized = 0;
        const auto load = [&](const char* name, std::vector<u8>& data) {
            if (use_opt && ReadFile(dir + "opt/" + name, data)) {
                ++optimized;
                return true;
            }
            if (ReadFile(dir + name, data)) {
                return true;
            }
            Fail("missing " + dir + name + " (run tools/fetch_fsr4_assets.sh)", true);
            return false;
        };
        if (!load(assets.pre, code[0])) return false;
        for (u32 pass = 0; pass < FFX_FSR4_MODEL_PASS_COUNT; ++pass) {
            if (!load(assets.model[pass], code[1 + pass])) return false;
        }
        if (!load(assets.post, code[13]) || !load(assets.rcas, code[14]) ||
            !load(assets.spdAutoExposure, code[15]) || !load(assets.initializer, initializer) ||
            !load(assets.prePassWeights, weights)) {
            return false;
        }
        if (initializer.size() != FFX_FSR4_V07_INITIALIZER_BYTES ||
            weights.size() != FFX_FSR4_V07_PRE_PASS_WEIGHTS_BYTES) {
            Fail("model weights have an unexpected size", true);
            return false;
        }
        if (optimized) {
            std::printf("Upscaler: FSR 4, %u passes from %sopt/\n", optimized, dir.c_str());
        }
        static const std::array<std::string, FFX_FSR4_VK_PASS_COUNT> entries = [] {
            std::array<std::string, FFX_FSR4_VK_PASS_COUNT> names;
            names.fill("main");
            for (u32 pass = 1; pass <= FFX_FSR4_MODEL_PASS_COUNT; ++pass) {
                names[pass] = "fsr4_model_v07_i8_pass" + std::to_string(pass);
            }
            return names;
        }();
        FfxFsr4VkCreateInfo ci{};
        ci.device = instance.GetDevice();
        ci.physicalDevice = instance.GetPhysicalDevice();
        for (u32 i = 0; i < FFX_FSR4_VK_PASS_COUNT; ++i) {
            if (code[i].size() % 4) {
                Fail("invalid SPIR-V asset", true);
                return false;
            }
            ci.shaders[i] = {reinterpret_cast<const uint32_t*>(code[i].data()), code[i].size(),
                             entries[i].c_str()};
        }
        ci.modelInitializer = initializer.data();
        ci.modelInitializerSize = initializer.size();
        ci.prePassWeights = weights.data();
        ci.prePassWeightsSize = weights.size();
        scratch.assign(ffxFsr4VkGetScratchMemorySize(), 0);
        ci.scratchBuffer = scratch.data();
        ci.scratchBufferSize = scratch.size();
        if (const VkResult result = ffxFsr4VkCreateContext(&ci, &backend);
            result != VK_SUCCESS) {
            Fail("Vulkan backend creation failed (" + std::to_string(int(result)) + ")", true);
            backend = {};
            return false;
        }
        backend_ok = true;

        ffxCreateContextDescUpscale desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
        if (preset == 5) desc.flags |= FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
        // Any preset's render size fits: the context is not recreated for a size change.
        desc.maxRenderSize = {(ow + 7) & ~7u, (oh + 7) & ~7u};
        desc.maxUpscaleSize = {(ow + 7) & ~7u, (oh + 7) & ~7u};
        ffxFsr4V07SetBackendInterface(&backend);
        const auto created = ffxFsr4V07CreateContext(&context, &desc.header, nullptr);
        ffxFsr4V07SetBackendInterface(nullptr);
        if (created != FFX_API_RETURN_OK) {
            Fail("provider context creation failed (" + std::to_string(created) + ")", true);
            return false;
        }
        context_ok = true;
        model = preset;
        out_width = ow;
        out_height = oh;
        std::printf("Upscaler: FSR 4 v07 INT8 %s model, %s tier, output %ux%u\n",
                    ffxFsr4ModelPresetName(ModelPreset(preset)), assets.tier, ow, oh);
        return true;
    }

    void Retire() {
        while (!in_flight.empty() && scheduler.IsFree(in_flight.front().second)) {
            ffxFsr4VkRetireFrame(&backend, in_flight.front().first);
            in_flight.pop_front();
        }
    }

    VkResult Register(const Image& image, VkAccessFlags access) {
        const FfxFsr4VkExternalImageState state{
            .structSize = sizeof(FfxFsr4VkExternalImageState),
            .image = image.image,
            .view = image.view,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
            .stageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            .accessMask = access,
            .restoreLayout = VK_IMAGE_LAYOUT_GENERAL,
            .restoreStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            .restoreAccessMask = access,
        };
        return ffxFsr4VkSetExternalImageState(&backend, &state);
    }

    /// Whether this frame tries the DLL model; sets dll_note when the user chose it and it
    /// cannot run.
    bool WantDll(const Frame& f) {
        if (BbSettings::Fsr4PreferredModel() != BbSettings::Fsr4ModelDll) {
            return false;
        }
        const bool chosen = BbSettings::Get().fsr4_model == BbSettings::Fsr4ModelDll;
        if (!instance.IsFsr4DllSupported()) {
            dll_note = chosen ? "the GPU lacks VK_VALVE_shader_mixed_float_dot_product; using FSR " +
                                    sdk_version
                              : std::string{};
            return false;
        }
        if (dll_failed_width == f.output.width && dll_failed_height == f.output.height) {
            return false;
        }
        std::error_code ec;
        return chosen || std::filesystem::is_directory(DllAssetDir(), ec);
    }

    bool Record411(const Frame& f) {
        if (!fsr411) {
            fsr411 = std::make_unique<Fsr411::Upscaler>(instance.GetPhysicalDevice(), instance.GetDevice(),
                                                        DllAssetDir());
        }
        // Its constant ring holds kFramesInFlight frames: the oldest must be done.
        while (fsr411_ticks.size() >= Fsr411::kFramesInFlight) {
            scheduler.Wait(fsr411_ticks.front());
            fsr411_ticks.pop_front();
        }
        Fsr411::Frame g;
        g.cmdbuf = f.cmdbuf;
        g.color = {f.color.image, f.color.view, f.color.width, f.color.height};
        g.depth = {f.depth.image, f.depth.view, f.depth.width, f.depth.height};
        g.motion = {f.motion.image, f.motion.view, f.motion.width, f.motion.height};
        g.output = {f.output.image, f.output.view, f.output.width, f.output.height};
        g.render_width = f.render_width;
        g.render_height = f.render_height;
        g.ultra_performance = f.preset >= 4;
        g.jitter[0] = f.jitter[0];
        g.jitter[1] = f.jitter[1];
        g.sharpness = f.sharpness;
        g.sharpen = f.sharpen && f.sharpness > 0.0f;
        g.reset = f.reset;
        g.auto_exposure = f.auto_exposure;
        if (!fsr411->Record(g)) {
            // v07 runs instead until the output size changes; the menu shows why if chosen.
            dll_failed_width = f.output.width;
            dll_failed_height = f.output.height;
            std::printf("Upscaler: FSR 4 DLL model unavailable (%s); using FSR %s\n",
                        fsr411->Error().c_str(), sdk_version.c_str());
            dll_note = BbSettings::Get().fsr4_model == BbSettings::Fsr4ModelDll
                           ? fsr411->Error() + "; using FSR " + sdk_version
                           : "";
            return false;
        }
        fsr411_ticks.push_back(scheduler.CurrentTick());
        if (const std::string d = fsr411->Describe(); d != fsr411_described) {
            fsr411_described = d;
            std::printf("Upscaler: FSR 4 DLL model %s, %s\n",
                        dll_version.empty() ? "(version unknown)" : dll_version.c_str(), d.c_str());
        }
        problem.clear();
        dll_note.clear();
        BbSettings::Get().fsr4_model_active = BbSettings::Fsr4ModelDll;
        return true;
    }

    bool Record(const Frame& f) {
        if (WantDll(f) && Record411(f)) {
            return true;
        }
        if (fatal) {
            return false;
        }
        if (!instance.IsFsr4Int8Supported()) {
            Fail("the GPU lacks INT8 dot product / compute derivative support", true);
            return false;
        }
        const int preset = f.dynamic_resolution ? 5 : std::clamp(f.preset, 0, 4);
        if (!context_ok || preset != model || f.output.width != out_width ||
            f.output.height != out_height) {
            Destroy();
            if (!CreateBackend(preset, f.output.width, f.output.height)) {
                Destroy();
                return false;
            }
        }
        Retire();
        const u64 frame_id = next_frame_id++;
        VkResult begin = ffxFsr4VkBeginFrame(&backend, frame_id);
        while (begin == VK_NOT_READY && !in_flight.empty()) {
            scheduler.Wait(in_flight.front().second);
            Retire();
            begin = ffxFsr4VkBeginFrame(&backend, frame_id);
        }
        if (begin != VK_SUCCESS) {
            Fail("no free provider frame (" + std::to_string(int(begin)) + ")", false);
            return false;
        }
        // bbport: a frame begun here must reach RetireFrame even when it records nothing,
        // or every later BeginFrame fails (VK_ERROR_VALIDATION_FAILED_EXT).
        const auto abandon = [&] { in_flight.emplace_back(frame_id, scheduler.CurrentTick()); };
        constexpr VkAccessFlags read = VK_ACCESS_SHADER_READ_BIT;
        const std::array<std::pair<const Image*, VkAccessFlags>, 4> images{{
            {&f.color, read},
            {&f.depth, read},
            {&f.motion, read},
            {&f.output, read | VK_ACCESS_SHADER_WRITE_BIT},
        }};
        static constexpr const char* names[] = {"color", "depth", "motion", "output"};
        for (u32 i = 0; i < images.size(); ++i) {
            if (const VkResult result = Register(*images[i].first, images[i].second);
                result != VK_SUCCESS) {
                Fail(std::string{"external image registration failed ("} + names[i] + ", " +
                         std::to_string(int(result)) + ")",
                     false);
                abandon();
                return false;
            }
        }
        const auto resource = [](const Image& image, u32 format, u32 state) {
            FfxApiResource r{};
            r.resource = reinterpret_cast<void*>(VkImageView(image.view));
            r.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
            r.description.format = format;
            r.description.width = image.width;
            r.description.height = image.height;
            r.description.depth = 1;
            r.description.mipCount = 1;
            r.state = state;
            return r;
        };
        ffxDispatchDescUpscale d{};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = reinterpret_cast<void*>(VkCommandBuffer(f.cmdbuf));
        d.color = resource(f.color, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT,
                           FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth = resource(f.depth, FFX_SURFACE_FORMAT_R32_FLOAT,
                           FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.motionVectors = resource(f.motion, FFX_SURFACE_FORMAT_R16G16_FLOAT,
                                   FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.output = resource(f.output, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT,
                            FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.jitterOffset = {f.jitter[0], f.jitter[1]};
        // Vectors are in render pixels; the provider divides by the render size.
        d.motionVectorScale = {1.0f, 1.0f};
        d.renderSize = {f.render_width, f.render_height};
        d.upscaleSize = {f.output.width, f.output.height};
        d.enableSharpening = f.sharpen && f.sharpness > 0.0f;
        d.sharpness = f.sharpness;
        d.enableAutoExposure = f.auto_exposure;
        d.frameTimeDelta = f.frame_ms;
        d.preExposure = 1.0f;
        d.reset = f.reset;
        d.cameraNear = f.near_plane;
        d.cameraFar = f.far_plane;
        d.cameraFovAngleVertical = f.vertical_fov;
        d.viewSpaceToMetersFactor = 1.0f;
        if (const auto result = ffxFsr4V07Dispatch(&context, &d.header);
            result != FFX_API_RETURN_OK) {
            Fail("dispatch failed (" + std::to_string(result) + ")", false);
            abandon();
            return false;
        }
        in_flight.emplace_back(frame_id, scheduler.CurrentTick());
        problem = dll_note; // v07 runs; if the DLL model was chosen, the menu says why not
        BbSettings::Get().fsr4_model_active = BbSettings::Fsr4ModelSdk;
        return true;
    }
};

void Fsr4Upscaler::PublishModels() {
    // The bundled model without a manifest (fetched before manifests): its pinned source, the
    // SDK's upscaler 4.0.2 (tools/fetch_fsr4_assets.sh).
    sdk_version = ManifestVersion(AssetDir());
    if (sdk_version.empty()) {
        sdk_version = "4.0.2";
    }
    auto& settings = BbSettings::Get();
    settings.fsr4_sdk_version = sdk_version.c_str();
    std::error_code ec;
    const std::string dir = DllAssetDir();
    if (std::filesystem::is_directory(dir, ec)) {
        dll_version = ManifestVersion(dir);
        settings.fsr4_dll_version = dll_version.c_str();
    }
    std::printf("Upscaler: FSR 4 models: %s (bundled)%s%s%s\n", sdk_version.c_str(),
                settings.fsr4_dll_version.load() ? ", " : "",
                settings.fsr4_dll_version.load() ? (dll_version.empty() ? "DLL model, version unknown"
                                                                         : dll_version.c_str())
                                                 : "",
                settings.fsr4_dll_version.load() ? (" in " + dir).c_str() : "");
}

Fsr4Upscaler::Fsr4Upscaler(const Instance& instance, Scheduler& scheduler)
    : impl{std::make_unique<Impl>(instance, scheduler)} {}

Fsr4Upscaler::~Fsr4Upscaler() = default;

bool Fsr4Upscaler::Record(const Frame& frame) {
    return impl->Record(frame);
}

const char* Fsr4Upscaler::Problem() const noexcept {
    return impl->problem.empty() ? nullptr : impl->problem.c_str();
}

bool Fsr4Upscaler::Fatal() const noexcept {
    return impl->fatal;
}

} // namespace Vulkan
