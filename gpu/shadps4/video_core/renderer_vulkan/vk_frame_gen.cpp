// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_frame_gen.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#ifndef _WIN32

namespace Vulkan::FrameGen {
bool Init(const Instance&, void*) {
    return false;
}
bool Active() {
    return false;
}
void Shutdown() {}
void Resize(u32, u32) {}
u32 ImageCount() {
    return 0;
}
vk::Format Format() {
    return vk::Format::eUndefined;
}
vk::Image Image(u32) {
    return {};
}
vk::ImageView View(u32) {
    return {};
}
u32 Acquire() {
    return 0;
}
void AddPresentSignal(SubmitInfo&, u32) {}
bool Present(u32, int) {
    return false;
}
void RecordInputs(vk::CommandBuffer, const Inputs&) {}
int TakeInputs() {
    return -1;
}
} // namespace Vulkan::FrameGen

#else

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <vulkan/vulkan_win32.h>

#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_matrix_helpers.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

#include "bbport_settings.h"
#include "core/emulator_settings.h"
#include "video_core/renderer_vulkan/vk_instance.h"

namespace Vulkan::FrameGen {

namespace {

constexpr u32 kImages = 3;    ///< DXGI back buffers and the shared images the presenter draws into
constexpr u32 kInputSets = 3; ///< frames of inputs in flight
constexpr DXGI_FORMAT kPresentFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr unsigned kNgxApplicationId = 231313132; // NVIDIA's id for unregistered applications
/// InputSet::consumed while the present thread holds the set (taken at the flip).
constexpr u64 kTaken = ~0ull;

template <typename T>
void Release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

/// A D3D12 texture and the Vulkan image bound to its memory.
struct Shared {
    ID3D12Resource* resource = nullptr;
    vk::Image image{};
    vk::DeviceMemory memory{};
    vk::ImageView view{};
    u32 width = 0, height = 0;
    DXGI_FORMAT dxgi_format = DXGI_FORMAT_UNKNOWN;
};

struct InputSet {
    Shared depth, motion, hudless;
    vk::Buffer staging{}; ///< depth aspect -> R32F (no depth-to-color image copy in Vulkan)
    vk::DeviceMemory staging_memory{};
    u32 render_width = 0, render_height = 0, out_width = 0, out_height = 0;
    std::atomic<u64> consumed{0}; ///< D3D12 fence value once its copies ran (0: never used)
    sl::Constants constants{};
    bool has_hudless = false;
};

struct State {
    const Instance* instance = nullptr;
    vk::Device device{};
    HWND hwnd{};
    HMODULE mod = nullptr, interposer = nullptr;

    PFun_slInit* slInit{};
    PFun_slShutdown* slShutdown{};
    PFun_slIsFeatureSupported* slIsFeatureSupported{};
    PFun_slGetFeatureFunction* slGetFeatureFunction{};
    PFun_slGetNewFrameToken* slGetNewFrameToken{};
    PFun_slSetTagForFrame* slSetTagForFrame{};
    PFun_slSetConstants* slSetConstants{};
    PFun_slSetD3DDevice* slSetD3DDevice{};
    PFun_slGetNativeInterface* slGetNativeInterface{};
    PFun_slDLSSGSetOptions* slDLSSGSetOptions{};
    PFun_slDLSSGGetState* slDLSSGGetState{};
    PFun_slReflexSetOptions* slReflexSetOptions{};
    PFun_slPCLSetMarker* slPCLSetMarker{};
    bool sl_initialized = false;

    IDXGIFactory6* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    ID3D12Device* d3d = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain3* swapchain = nullptr;
    ID3D12CommandAllocator* allocators[kImages]{};
    u64 allocator_value[kImages]{};
    ID3D12GraphicsCommandList* list = nullptr;
    bool tearing = false;

    /// Signalled by Vulkan (the presenter's submission), waited on by the D3D12 queue.
    ID3D12Fence* vk_fence = nullptr;
    vk::Semaphore vk_semaphore{};
    u64 vk_value = 0;
    /// Signalled by the D3D12 queue after its copies, waited on by the CPU.
    ID3D12Fence* d3d_fence = nullptr;
    std::atomic<u64> d3d_value{0};

    Shared present[kImages];
    u64 present_vk_value[kImages]{};
    u64 present_d3d_value[kImages]{};
    u32 next_image = 0;
    u32 width = 0, height = 0;
    u32 frame_counter = 0;

    InputSet sets[kInputSets];
    u32 next_set = 0;
    std::mutex latest_mutex;
    int latest = -1;
    std::vector<Shared> retired; ///< replaced on a size change; freed at shutdown

    sl::DLSSGOptions applied{};
    bool options_set = false;
    u32 presents_without_inputs = 0;
    u32 reflex_limit_us = ~0u;
    std::chrono::steady_clock::time_point last_status{};
};

std::unique_ptr<State> g;
std::string problem_text; ///< BbSettings::frame_gen_problem points here

void SetProblem(const std::string& text) {
    std::printf("Frame generation: %s\n", text.c_str());
    problem_text = text;
    BbSettings::Get().frame_gen_problem = problem_text.c_str();
}

std::wstring ExeDir() {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return n ? std::filesystem::path(std::wstring(exe, n)).parent_path().wstring() : L".";
}

const char* SlResult(sl::Result r) {
    switch (r) {
    case sl::Result::eOk:
        return "ok";
    case sl::Result::eErrorNotInitialized:
        return "not initialized";
    case sl::Result::eErrorFeatureMissing:
        return "feature missing (plugin not loaded)";
    case sl::Result::eErrorFeatureNotSupported:
        return "feature not supported";
    case sl::Result::eErrorAdapterNotSupported:
        return "adapter not supported (RTX 40 or newer, or the dlssg_sm86 mod on RTX 20/30)";
    case sl::Result::eErrorDriverOutOfDate:
        return "driver out of date";
    case sl::Result::eErrorOSOutOfDate:
        return "Windows too old";
    case sl::Result::eErrorOSDisabledHWS:
        return "hardware-accelerated GPU scheduling is off (Windows graphics settings)";
    case sl::Result::eErrorNoSupportedAdapterFound:
        return "no supported adapter";
    case sl::Result::eErrorMissingOrInvalidAPI:
        return "missing or invalid API";
    case sl::Result::eErrorFeatureFailedToLoad:
        return "feature failed to load";
    default: {
        static char text[32];
        std::snprintf(text, sizeof(text), "error %d", int(r));
        return text;
    }
    }
}

void LogCallback(sl::LogType type, const char* message) {
    if (type == sl::LogType::eInfo && !std::getenv("BB_SL_LOG")) {
        return;
    }
    std::printf("Streamline: %s%s", message,
                *message && message[std::strlen(message) - 1] == '\n' ? "" : "\n");
}

void WaitD3D(u64 value) {
    if (value && g->d3d_fence->GetCompletedValue() < value) {
        g->d3d_fence->SetEventOnCompletion(value, nullptr); // blocks
    }
}

u32 MemoryType(u32 bits) {
    const auto props = g->instance->GetPhysicalDevice().getMemoryProperties();
    for (u32 i = 0; i < props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
            return i;
        }
    }
    for (u32 i = 0; i < props.memoryTypeCount; ++i) {
        if (bits & (1u << i)) {
            return i;
        }
    }
    return 0;
}

void Destroy(Shared& s) {
    if (s.view) {
        g->device.destroyImageView(s.view);
    }
    if (s.image) {
        g->device.destroyImage(s.image);
    }
    if (s.memory) {
        g->device.freeMemory(s.memory);
    }
    Release(s.resource);
    s = {};
}

/// A D3D12 texture shared with Vulkan (simultaneous access: no D3D12 state transitions).
bool Create(Shared& s, u32 width, u32 height, DXGI_FORMAT dxgi, vk::Format format,
            vk::ImageUsageFlags usage, bool render_target, const char* name) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = dxgi;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                 (render_target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
                                : D3D12_RESOURCE_FLAG_NONE);
    HRESULT hr = g->d3d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                                                D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(&s.resource));
    HANDLE handle = nullptr;
    if (SUCCEEDED(hr)) {
        hr = g->d3d->CreateSharedHandle(s.resource, nullptr, GENERIC_ALL, nullptr, &handle);
    }
    if (FAILED(hr)) {
        std::printf("Frame generation: D3D12 %s %ux%u failed (0x%08lx)\n", name, width, height,
                    hr);
        Release(s.resource);
        return false;
    }
    s.width = width;
    s.height = height;
    s.dxgi_format = dxgi;

    const vk::ExternalMemoryImageCreateInfo external{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eD3D12Resource,
    };
    const auto [image_result, image] = g->device.createImage({
        .pNext = &external,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    if (image_result != vk::Result::eSuccess) {
        CloseHandle(handle);
        std::printf("Frame generation: Vulkan image for %s failed\n", name);
        Destroy(s);
        return false;
    }
    s.image = image;
    const auto requirements = g->device.getImageMemoryRequirements(image);
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = VkImage(image);
    VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    import.pNext = &dedicated;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    import.handle = handle;
    const auto [memory_result, memory] = g->device.allocateMemory({
        .pNext = &import,
        .allocationSize = requirements.size,
        .memoryTypeIndex = MemoryType(requirements.memoryTypeBits),
    });
    // Importing does not take the NT handle over.
    CloseHandle(handle);
    if (memory_result != vk::Result::eSuccess ||
        g->device.bindImageMemory(image, memory, 0) != vk::Result::eSuccess) {
        std::printf("Frame generation: importing %s into Vulkan failed\n", name);
        if (memory_result == vk::Result::eSuccess) {
            g->device.freeMemory(memory);
        }
        Destroy(s);
        return false;
    }
    s.memory = memory;
    const auto [view_result, view] = g->device.createImageView({
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    });
    s.view = view_result == vk::Result::eSuccess ? view : vk::ImageView{};
    return true;
}

template <typename Fn>
bool Export(Fn*& fn, const char* name) {
    fn = reinterpret_cast<Fn*>(GetProcAddress(g->interposer, name));
    return fn != nullptr;
}

template <typename Fn>
bool FeatureFunction(sl::Feature feature, Fn*& fn, const char* name) {
    void* p = nullptr;
    if (g->slGetFeatureFunction(feature, name, p) != sl::Result::eOk || !p) {
        return false;
    }
    fn = reinterpret_cast<Fn*>(p);
    return true;
}

/// Bloodborne's projection (clip x = xs x, y = ys y, z = zs z + zo, w = z) for row vectors.
sl::float4x4 ViewToClip(const std::array<float, 4>& p) {
    return {sl::float4(p[0], 0, 0, 0), sl::float4(0, p[1], 0, 0), sl::float4(0, 0, p[2], 1),
            sl::float4(0, 0, p[3], 0)};
}

/// A 3x4 row matrix for column vectors as a 4x4 matrix for row vectors.
sl::float4x4 RowVector(const std::array<float, 12>& m) {
    return {sl::float4(m[0], m[4], m[8], 0), sl::float4(m[1], m[5], m[9], 0),
            sl::float4(m[2], m[6], m[10], 0), sl::float4(m[3], m[7], m[11], 1)};
}

/// 3x4 rows a * b (column vectors).
std::array<float, 12> Multiply(const std::array<float, 12>& a, const std::array<float, 12>& b) {
    std::array<float, 12> r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            float v = j == 3 ? a[i * 4 + 3] : 0.0f;
            for (int k = 0; k < 3; ++k) {
                v += a[i * 4 + k] * b[k * 4 + j];
            }
            r[i * 4 + j] = v;
        }
    }
    return r;
}

sl::Constants MakeConstants(const Inputs& in) {
    const Camera& c = in.camera;
    sl::Constants k{};
    k.cameraViewToClip = ViewToClip(c.proj);
    sl::matrixFullInvert(k.clipToCameraView, k.cameraViewToClip);
    k.clipToLensClip = {sl::float4(1, 0, 0, 0), sl::float4(0, 1, 0, 0), sl::float4(0, 0, 1, 0),
                        sl::float4(0, 0, 0, 1)};
    const sl::float4x4 view_to_prev_view = RowVector(Multiply(c.prev_view, c.inv_view));
    sl::float4x4 clip_to_prev_view;
    sl::matrixMul(clip_to_prev_view, k.clipToCameraView, view_to_prev_view);
    sl::matrixMul(k.clipToPrevClip, clip_to_prev_view, ViewToClip(c.prev_proj));
    sl::matrixFullInvert(k.prevClipToClip, k.clipToPrevClip);
    k.jitterOffset = {in.jitter[0], in.jitter[1]};
    k.mvecScale = {1.0f / float(in.render_width), 1.0f / float(in.render_height)};
    k.cameraPinholeOffset = {0.0f, 0.0f};
    const auto& iv = c.inv_view; // view to world: columns are right, up, forward, position
    k.cameraRight = {iv[0], iv[4], iv[8]};
    k.cameraUp = {iv[1], iv[5], iv[9]};
    k.cameraFwd = {iv[2], iv[6], iv[10]};
    k.cameraPos = {iv[3], iv[7], iv[11]};
    k.cameraNear = c.proj[2] != 0.0f ? -c.proj[3] / c.proj[2] : 0.05f;
    k.cameraFar = 3000.0f;
    k.cameraFOV = 2.0f * std::atan(1.0f / c.proj[1]);
    k.cameraAspectRatio = c.proj[1] / c.proj[0];
    k.motionVectorsInvalidValue = 0.0f;
    k.depthInverted = sl::Boolean::eFalse;
    k.cameraMotionIncluded = sl::Boolean::eTrue;
    k.motionVectors3D = sl::Boolean::eFalse;
    k.reset = in.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    k.orthographicProjection = sl::Boolean::eFalse;
    k.motionVectorsDilated = sl::Boolean::eFalse;
    k.motionVectorsJittered = sl::Boolean::eFalse;
    return k;
}

/// Generated frames per rendered frame for the menu choice (dynamic: the most it may use).
u32 GeneratedFrames(int mode) {
    switch (mode) {
    case BbSettings::FrameGen2x:
        return 1;
    case BbSettings::FrameGen3x:
        return 2;
    default:
        return 3;
    }
}

/// Reflex caps the rendered frame rate at the display rate divided by the multiplier, so the
/// output stays within the refresh rate; dynamic mode picks its multiplier itself.
void ApplyReflex(int mode) {
    const u32 hz = BbDisplayRefreshHz();
    const u32 limit_us = mode == BbSettings::FrameGenDynamic || mode == BbSettings::FrameGenOff
                             ? 0
                             : u32(1e6 * (GeneratedFrames(mode) + 1) / double(hz));
    if (limit_us == g->reflex_limit_us || !g->slReflexSetOptions) {
        return;
    }
    sl::ReflexOptions options{};
    options.mode = sl::ReflexMode::eLowLatency;
    options.frameLimitUs = limit_us;
    if (g->slReflexSetOptions(options) == sl::Result::eOk) {
        g->reflex_limit_us = limit_us;
        std::printf("Frame generation: Reflex on, rendered frames limited to %s\n",
                    limit_us ? (std::to_string(1000000 / limit_us) + " FPS").c_str() : "none");
    }
}

void DestroyImages() {
    for (auto& s : g->present) {
        Destroy(s);
    }
}

bool CreateImages(u32 width, u32 height) {
    for (u32 i = 0; i < kImages; ++i) {
        if (!Create(g->present[i], width, height, kPresentFormat, vk::Format::eR8G8B8A8Unorm,
                    vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                        vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
                    true, "present image")) {
            return false;
        }
        g->present_vk_value[i] = g->present_d3d_value[i] = 0;
    }
    g->width = width;
    g->height = height;
    return true;
}

void Teardown() {
    if (!g) {
        return;
    }
    if (g->queue && g->d3d_fence) {
        const u64 value = ++g->d3d_value;
        g->queue->Signal(g->d3d_fence, value);
        WaitD3D(value);
    }
    if (g->device) {
        (void)g->device.waitIdle();
    }
    DestroyImages();
    for (auto& set : g->sets) {
        Destroy(set.depth);
        Destroy(set.motion);
        Destroy(set.hudless);
        if (set.staging) {
            g->device.destroyBuffer(set.staging);
        }
        if (set.staging_memory) {
            g->device.freeMemory(set.staging_memory);
        }
    }
    for (auto& s : g->retired) {
        Destroy(s);
    }
    if (g->vk_semaphore) {
        g->device.destroySemaphore(g->vk_semaphore);
    }
    Release(g->list);
    for (auto*& a : g->allocators) {
        Release(a);
    }
    Release(g->swapchain);
    Release(g->vk_fence);
    Release(g->d3d_fence);
    Release(g->queue);
    Release(g->d3d);
    Release(g->adapter);
    Release(g->factory);
    if (g->sl_initialized && g->slShutdown) {
        g->slShutdown();
    }
    g.reset();
}

bool Setup(const Instance& instance, HWND hwnd) {
    g = std::make_unique<State>();
    g->instance = &instance;
    g->device = instance.GetDevice();
    g->hwnd = hwnd;
    if (!instance.IsExternalWin32Supported()) {
        SetProblem("the Vulkan driver cannot share images with D3D12 (VK_KHR_external_memory_win32)");
        return false;
    }

    // The dlssg_sm86 mod (RTX 20/30) must be in place before Streamline asks NVAPI for the GPU
    // architecture in slInit.
    std::wstring mod_path = ExeDir() + L"\\dlssg_sm86\\version.dll";
    if (const char* env = std::getenv("BB_DLSSG_MOD"); env && *env) {
        mod_path = std::filesystem::path(env).wstring();
    }
    if (mod_path != L"0" && std::filesystem::exists(mod_path)) {
        g->mod = LoadLibraryW(mod_path.c_str());
        std::printf("Frame generation: dlssg_sm86 mod %s (%ls)\n", g->mod ? "loaded" : "failed to load",
                    mod_path.c_str());
    }

    std::wstring sl_dir = ExeDir() + L"\\streamline";
    if (const char* env = std::getenv("BB_STREAMLINE_DIR"); env && *env) {
        sl_dir = std::filesystem::path(env).wstring();
    }
    g->interposer = LoadLibraryW((sl_dir + L"\\sl.interposer.dll").c_str());
    if (!g->interposer) {
        SetProblem("Streamline not found (out\\streamline\\sl.interposer.dll; tools/fetch_streamline.sh)");
        return false;
    }
    using PFN_CreateFactory = HRESULT(WINAPI*)(UINT, REFIID, void**);
    using PFN_CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    PFN_CreateFactory create_factory =
        reinterpret_cast<PFN_CreateFactory>(GetProcAddress(g->interposer, "CreateDXGIFactory2"));
    PFN_CreateDevice create_device =
        reinterpret_cast<PFN_CreateDevice>(GetProcAddress(g->interposer, "D3D12CreateDevice"));
    if (!create_factory || !create_device || !Export(g->slInit, "slInit") ||
        !Export(g->slShutdown, "slShutdown") ||
        !Export(g->slIsFeatureSupported, "slIsFeatureSupported") ||
        !Export(g->slGetFeatureFunction, "slGetFeatureFunction") ||
        !Export(g->slGetNewFrameToken, "slGetNewFrameToken") ||
        !Export(g->slSetTagForFrame, "slSetTagForFrame") ||
        !Export(g->slSetConstants, "slSetConstants") ||
        !Export(g->slSetD3DDevice, "slSetD3DDevice") ||
        !Export(g->slGetNativeInterface, "slGetNativeInterface")) {
        SetProblem("sl.interposer.dll lacks the Streamline 2 entry points");
        return false;
    }

    const std::wstring plugin_dir = sl_dir;
    const wchar_t* plugin_paths[] = {plugin_dir.c_str()};
    const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences prefs{};
    prefs.showConsole = false;
    prefs.logLevel = std::getenv("BB_SL_LOG") ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    prefs.pathsToPlugins = plugin_paths;
    prefs.numPathsToPlugins = 1;
    prefs.logMessageCallback = &LogCallback;
    prefs.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                  sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    prefs.featuresToLoad = features;
    prefs.numFeaturesToLoad = 3;
    prefs.applicationId = kNgxApplicationId;
    prefs.engine = sl::EngineType::eCustom;
    prefs.engineVersion = "bbport";
    prefs.renderAPI = sl::RenderAPI::eD3D12;
    if (const sl::Result r = g->slInit(prefs, sl::kSDKVersion); r != sl::Result::eOk) {
        SetProblem(std::string("Streamline initialization failed: ") + SlResult(r));
        return false;
    }
    g->sl_initialized = true;

    if (FAILED(create_factory(0, IID_PPV_ARGS(&g->factory)))) {
        SetProblem("DXGI factory (through Streamline) failed");
        return false;
    }
    BOOL tearing = FALSE;
    g->tearing = SUCCEEDED(g->factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                           &tearing, sizeof(tearing))) &&
                 tearing;

    // The adapter the Vulkan device runs on.
    vk::PhysicalDeviceIDProperties id{};
    vk::PhysicalDeviceProperties2 props{.pNext = &id};
    instance.GetPhysicalDevice().getProperties2(&props);
    if (!id.deviceLUIDValid) {
        SetProblem("the Vulkan device reports no adapter LUID");
        return false;
    }
    LUID luid{};
    std::memcpy(&luid, id.deviceLUID.data(), sizeof(luid));
    if (FAILED(g->factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&g->adapter)))) {
        SetProblem("the Vulkan GPU is not a DXGI adapter");
        return false;
    }
    sl::AdapterInfo adapter_info{};
    adapter_info.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
    adapter_info.deviceLUIDSizeInBytes = sizeof(luid);
    if (const sl::Result r = g->slIsFeatureSupported(sl::kFeatureDLSS_G, adapter_info);
        r != sl::Result::eOk) {
        SetProblem(std::string("DLSS Frame Generation unavailable: ") + SlResult(r));
        return false;
    }
    if (FAILED(create_device(g->adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g->d3d)))) {
        SetProblem("D3D12 device creation failed");
        return false;
    }
    // The plugins start on the device (the proxy hooks alone do not hand it over).
    void* native_device = g->d3d;
    g->slGetNativeInterface(g->d3d, &native_device);
    if (const sl::Result r = g->slSetD3DDevice(native_device); r != sl::Result::eOk) {
        SetProblem(std::string("Streamline did not take the D3D12 device: ") + SlResult(r));
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g->d3d->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&g->queue)))) {
        SetProblem("D3D12 queue creation failed");
        return false;
    }
    for (auto*& a : g->allocators) {
        if (FAILED(g->d3d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&a)))) {
            SetProblem("D3D12 command allocator creation failed");
            return false;
        }
    }
    if (FAILED(g->d3d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g->allocators[0],
                                         nullptr, IID_PPV_ARGS(&g->list)))) {
        SetProblem("D3D12 command list creation failed");
        return false;
    }
    g->list->Close();
    if (FAILED(g->d3d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g->d3d_fence))) ||
        FAILED(g->d3d->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g->vk_fence)))) {
        SetProblem("D3D12 fence creation failed");
        return false;
    }
    {
        HANDLE handle = nullptr;
        if (FAILED(g->d3d->CreateSharedHandle(g->vk_fence, nullptr, GENERIC_ALL, nullptr,
                                              &handle))) {
            SetProblem("sharing the D3D12 fence failed");
            return false;
        }
        const vk::SemaphoreTypeCreateInfo type{.semaphoreType = vk::SemaphoreType::eTimeline,
                                               .initialValue = 0};
        const auto [result, semaphore] = g->device.createSemaphore({.pNext = &type});
        auto import_fn = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
            g->device.getProcAddr("vkImportSemaphoreWin32HandleKHR"));
        VkImportSemaphoreWin32HandleInfoKHR import{
            VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
        import.semaphore = VkSemaphore(semaphore);
        import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        import.handle = handle;
        const bool ok = result == vk::Result::eSuccess && import_fn &&
                        import_fn(VkDevice(g->device), &import) == VK_SUCCESS;
        CloseHandle(handle);
        if (result == vk::Result::eSuccess) {
            g->vk_semaphore = semaphore;
        }
        if (!ok) {
            SetProblem("importing the D3D12 fence into Vulkan failed");
            return false;
        }
    }

    RECT client{};
    GetClientRect(hwnd, &client);
    const u32 width = std::max<LONG>(client.right - client.left, 64);
    const u32 height = std::max<LONG>(client.bottom - client.top, 64);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = kPresentFormat;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kImages;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc.Flags = g->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    IDXGISwapChain1* swapchain1 = nullptr;
    HRESULT hr = g->factory->CreateSwapChainForHwnd(g->queue, hwnd, &desc, nullptr, nullptr,
                                                    &swapchain1);
    if (SUCCEEDED(hr)) {
        hr = swapchain1->QueryInterface(IID_PPV_ARGS(&g->swapchain));
        swapchain1->Release();
    }
    if (FAILED(hr)) {
        SetProblem("DXGI swapchain creation failed (0x" + std::to_string(unsigned(hr)) + ")");
        return false;
    }
    g->factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (!CreateImages(width, height)) {
        SetProblem("shared present images could not be created");
        return false;
    }

    if (!FeatureFunction(sl::kFeatureDLSS_G, g->slDLSSGSetOptions, "slDLSSGSetOptions") ||
        !FeatureFunction(sl::kFeatureDLSS_G, g->slDLSSGGetState, "slDLSSGGetState")) {
        SetProblem("the DLSS-G plugin (sl.dlss_g.dll) did not load");
        return false;
    }
    FeatureFunction(sl::kFeatureReflex, g->slReflexSetOptions, "slReflexSetOptions");
    FeatureFunction(sl::kFeaturePCL, g->slPCLSetMarker, "slPCLSetMarker");
    ApplyReflex(BbSettings::Get().frame_gen);

    DXGI_ADAPTER_DESC1 adapter_desc{};
    g->adapter->GetDesc1(&adapter_desc);
    std::printf("Frame generation: DLSS-G ready on %ls, presenting through DXGI %ux%u%s\n",
                adapter_desc.Description, width, height, g->tearing ? " (tearing allowed)" : "");
    return true;
}

bool EnsureInputs(InputSet& s, const Inputs& in) {
    const bool render_changed = s.render_width != in.render_width ||
                                s.render_height != in.render_height;
    const bool out_changed = s.out_width != in.out_width || s.out_height != in.out_height;
    if (!render_changed && !out_changed && s.depth.image) {
        return true;
    }
    // Copies of this set may still be queued: keep the old images until shutdown.
    if (render_changed) {
        g->retired.push_back(s.depth);
        g->retired.push_back(s.motion);
        s.depth = {};
        s.motion = {};
        if (s.staging) {
            g->device.destroyBuffer(s.staging); // only touched by commands already waited for
            g->device.freeMemory(s.staging_memory);
            s.staging = vk::Buffer{};
            s.staging_memory = vk::DeviceMemory{};
        }
        const auto usage = vk::ImageUsageFlagBits::eTransferDst |
                           vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled;
        if (!Create(s.depth, in.render_width, in.render_height, DXGI_FORMAT_R32_FLOAT,
                    vk::Format::eR32Sfloat, usage, false, "depth") ||
            !Create(s.motion, in.render_width, in.render_height, DXGI_FORMAT_R16G16_FLOAT,
                    vk::Format::eR16G16Sfloat, usage, false, "motion vectors")) {
            return false;
        }
        const vk::DeviceSize size = vk::DeviceSize(in.render_width) * in.render_height * 4;
        const auto [buffer_result, buffer] = g->device.createBuffer({
            .size = size,
            .usage = vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc,
        });
        if (buffer_result != vk::Result::eSuccess) {
            return false;
        }
        s.staging = buffer;
        const auto req = g->device.getBufferMemoryRequirements(buffer);
        const auto [memory_result, memory] = g->device.allocateMemory(
            {.allocationSize = req.size, .memoryTypeIndex = MemoryType(req.memoryTypeBits)});
        if (memory_result != vk::Result::eSuccess ||
            g->device.bindBufferMemory(buffer, memory, 0) != vk::Result::eSuccess) {
            return false;
        }
        s.staging_memory = memory;
        s.render_width = in.render_width;
        s.render_height = in.render_height;
    }
    if (out_changed || !s.hudless.image) {
        g->retired.push_back(s.hudless);
        s.hudless = {};
        if (!Create(s.hudless, in.out_width, in.out_height, DXGI_FORMAT_R8G8B8A8_UNORM,
                    vk::Format::eR8G8B8A8Unorm,
                    vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc |
                        vk::ImageUsageFlagBits::eSampled,
                    false, "hud-less color")) {
            return false;
        }
        s.out_width = in.out_width;
        s.out_height = in.out_height;
    }
    return true;
}

void ReportStatus(const sl::DLSSGOptions& options) {
    const auto now = std::chrono::steady_clock::now();
    if (now - g->last_status < std::chrono::seconds(5)) {
        return;
    }
    g->last_status = now;
    sl::DLSSGState state{};
    if (g->slDLSSGGetState(sl::ViewportHandle{0}, state, &options) != sl::Result::eOk) {
        return;
    }
    auto& settings = BbSettings::Get();
    const bool on = options.mode != sl::DLSSGMode::eOff && state.status == sl::DLSSGStatus::eOk;
    settings.frame_gen_active = on;
    settings.frame_gen_presented = on ? std::max<int>(1, state.numFramesActuallyPresented) : 1;
    static std::string text;
    if (state.status != sl::DLSSGStatus::eOk) {
        const auto s = state.status;
        text = (s & sl::DLSSGStatus::eFailResolutionTooLow)       ? "resolution too low"
             : (s & sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime) ? "Reflex not detected"
             : (s & sl::DLSSGStatus::eFailHDRFormatNotSupported)  ? "HDR format not supported"
             : (s & sl::DLSSGStatus::eFailCommonConstantsInvalid) ? "invalid camera constants"
                                                                  : "DLSS-G status error";
        SetProblem(text);
    } else if (options.mode != sl::DLSSGMode::eOff) {
        settings.frame_gen_problem = nullptr;
    }
    if (std::getenv("BB_FRAME_STATS")) {
        std::printf("Frame generation: mode %d, %u frames presented per rendered frame "
                    "(max %u generated), status 0x%x, VRAM %.0f MiB\n",
                    int(options.mode), state.numFramesActuallyPresented,
                    state.numFramesToGenerateMax, unsigned(state.status),
                    state.estimatedVRAMUsageInBytes / 1048576.0);
    }
}

} // namespace

bool Init(const Instance& instance, void* hwnd) {
    auto& settings = BbSettings::Get();
    settings.frame_gen_ready = false;
    if (settings.startup_frame_gen == BbSettings::FrameGenOff) {
        return false;
    }
    if (!Setup(instance, static_cast<HWND>(hwnd))) {
        Teardown();
        std::printf("Frame generation: off; presenting through Vulkan\n");
        return false;
    }
    settings.frame_gen_ready = true;
    settings.frame_gen_problem = nullptr;
    return true;
}

bool Active() {
    return g != nullptr;
}

void Shutdown() {
    Teardown();
}

void Resize(u32 width, u32 height) {
    width = std::max<u32>(width, 64);
    height = std::max<u32>(height, 64);
    if (width == g->width && height == g->height) {
        return;
    }
    const u64 value = ++g->d3d_value;
    g->queue->Signal(g->d3d_fence, value);
    WaitD3D(value);
    (void)g->device.waitIdle();
    DestroyImages();
    const HRESULT hr = g->swapchain->ResizeBuffers(
        kImages, width, height, kPresentFormat, g->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(hr)) {
        std::printf("Frame generation: ResizeBuffers %ux%u failed (0x%08lx)\n", width, height, hr);
    }
    if (!CreateImages(width, height)) {
        std::printf("Frame generation: present images %ux%u failed\n", width, height);
    }
    g->options_set = false; // the output size is part of the options
}

u32 ImageCount() {
    return kImages;
}

vk::Format Format() {
    return vk::Format::eR8G8B8A8Unorm;
}

vk::Image Image(u32 index) {
    return g->present[index].image;
}

vk::ImageView View(u32 index) {
    return g->present[index].view;
}

u32 Acquire() {
    const u32 index = g->next_image;
    g->next_image = (index + 1) % kImages;
    WaitD3D(g->present_d3d_value[index]);
    return index;
}

void AddPresentSignal(SubmitInfo& info, u32 index) {
    g->present_vk_value[index] = ++g->vk_value;
    info.AddSignal(g->vk_semaphore, g->vk_value);
}

bool Present(u32 index, int inputs) {
    auto& settings = BbSettings::Get();
    const u32 frame = g->frame_counter++;
    const u32 a = frame % kImages;
    WaitD3D(g->allocator_value[a]);
    g->allocators[a]->Reset();
    g->list->Reset(g->allocators[a], nullptr);
    g->queue->Wait(g->vk_fence, g->present_vk_value[index]);

    ID3D12Resource* back = nullptr;
    g->swapchain->GetBuffer(g->swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back));
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = back;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g->list->ResourceBarrier(1, &barrier);
    g->list->CopyResource(back, g->present[index].resource);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    g->list->ResourceBarrier(1, &barrier);
    Release(back);

    const int mode = settings.frame_gen;
    const bool have_inputs = inputs >= 0 && inputs < int(kInputSets);
    if (have_inputs && g->sets[inputs].consumed != kTaken) {
        inputs = -1; // recorded again meanwhile (the bound above ran out): not this frame's
    }
    g->presents_without_inputs = have_inputs ? 0 : g->presents_without_inputs + 1;
    // Menus and loading screens have no scene: generation pauses (its resources are kept).
    const bool generate = mode != BbSettings::FrameGenOff && g->presents_without_inputs < 2;
    const bool tag = have_inputs && inputs >= 0;

    sl::FrameToken* token = nullptr;
    const uint32_t frame_index = frame;
    g->slGetNewFrameToken(token, &frame_index);
    const sl::ViewportHandle viewport{0};
    if (token && g->slPCLSetMarker) {
        g->slPCLSetMarker(sl::PCLMarker::eSimulationStart, *token);
        g->slPCLSetMarker(sl::PCLMarker::eSimulationEnd, *token);
        g->slPCLSetMarker(sl::PCLMarker::eRenderSubmitStart, *token);
    }

    sl::DLSSGOptions options{};
    options.mode = !generate ? sl::DLSSGMode::eOff
                   : mode == BbSettings::FrameGenDynamic ? sl::DLSSGMode::eDynamic
                                                         : sl::DLSSGMode::eOn;
    options.numFramesToGenerate = GeneratedFrames(mode);
    options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    options.numBackBuffers = kImages;
    options.colorWidth = g->width;
    options.colorHeight = g->height;
    options.colorBufferFormat = kPresentFormat;
    options.mvecBufferFormat = DXGI_FORMAT_R16G16_FLOAT;
    options.depthBufferFormat = DXGI_FORMAT_R32_FLOAT;
    options.hudLessBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    options.dynamicTargetFrameRate = float(BbDisplayRefreshHz());
    if (tag) {
        const InputSet& set = g->sets[inputs];
        options.mvecDepthWidth = set.render_width;
        options.mvecDepthHeight = set.render_height;
    } else {
        options.mvecDepthWidth = g->applied.mvecDepthWidth;
        options.mvecDepthHeight = g->applied.mvecDepthHeight;
    }
    const auto& a0 = g->applied;
    if (!g->options_set || a0.mode != options.mode ||
        a0.numFramesToGenerate != options.numFramesToGenerate ||
        a0.colorWidth != options.colorWidth || a0.colorHeight != options.colorHeight ||
        a0.mvecDepthWidth != options.mvecDepthWidth ||
        a0.mvecDepthHeight != options.mvecDepthHeight) {
        const sl::Result r = g->slDLSSGSetOptions(viewport, options);
        if (r != sl::Result::eOk) {
            SetProblem(std::string("DLSS-G options rejected: ") + SlResult(r));
        } else if (!g->options_set || a0.mode != options.mode ||
                   a0.numFramesToGenerate != options.numFramesToGenerate) {
            std::printf("Frame generation: %s\n",
                        options.mode == sl::DLSSGMode::eOff ? "paused (no scene)"
                                                            : BbSettings::FrameGenLabel(mode));
        }
        g->applied = options;
        g->options_set = true;
        ApplyReflex(generate ? mode : BbSettings::FrameGenOff);
    }

    if (token && tag) {
        InputSet& set = g->sets[inputs];
        g->slSetConstants(set.constants, *token, viewport);
        sl::Resource depth{sl::ResourceType::eTex2d, set.depth.resource, D3D12_RESOURCE_STATE_COMMON};
        sl::Resource motion{sl::ResourceType::eTex2d, set.motion.resource,
                            D3D12_RESOURCE_STATE_COMMON};
        sl::Resource hudless{sl::ResourceType::eTex2d, set.hudless.resource,
                             D3D12_RESOURCE_STATE_COMMON};
        const sl::Extent render{0, 0, set.render_width, set.render_height};
        const sl::Extent output{0, 0, set.out_width, set.out_height};
        sl::ResourceTag tags[] = {
            sl::ResourceTag{&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow,
                            &render},
            sl::ResourceTag{&motion, sl::kBufferTypeMotionVectors,
                            sl::ResourceLifecycle::eOnlyValidNow, &render},
            sl::ResourceTag{&hudless, sl::kBufferTypeHUDLessColor,
                            sl::ResourceLifecycle::eOnlyValidNow, &output},
        };
        g->slSetTagForFrame(*token, viewport, tags, set.has_hudless ? 3u : 2u, g->list);
    }

    g->list->Close();
    ID3D12CommandList* lists[] = {g->list};
    g->queue->ExecuteCommandLists(1, lists);
    const u64 done = ++g->d3d_value;
    g->queue->Signal(g->d3d_fence, done);
    g->allocator_value[a] = done;
    g->present_d3d_value[index] = done;
    if (tag) {
        g->sets[inputs].consumed = done;
    }

    if (token && g->slPCLSetMarker) {
        g->slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd, *token);
        g->slPCLSetMarker(sl::PCLMarker::ePresentStart, *token);
    }
    const HRESULT hr = g->swapchain->Present(0, g->tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (token && g->slPCLSetMarker) {
        g->slPCLSetMarker(sl::PCLMarker::ePresentEnd, *token);
    }
    if (FAILED(hr)) {
        static HRESULT reported = S_OK;
        if (reported != hr) {
            reported = hr;
            std::printf("Frame generation: Present failed (0x%08lx)\n", hr);
        }
    }
    ReportStatus(options);
    return SUCCEEDED(hr);
}

void RecordInputs(vk::CommandBuffer cmdbuf, const Inputs& in) {
    if (!g || BbSettings::Get().frame_gen == BbSettings::FrameGenOff || !in.depth ||
        !in.motion || in.render_width == 0 || in.render_height == 0) {
        return;
    }
    const u32 j = g->next_set;
    g->next_set = (j + 1) % kInputSets;
    InputSet& s = g->sets[j];
    // Taken at a flip but not presented yet: Present gives the fence value (a skipped frame
    // never does, hence the bound).
    for (int i = 0; s.consumed == kTaken && i < 100; ++i) {
        Sleep(1);
    }
    if (const u64 consumed = s.consumed; consumed != kTaken) {
        WaitD3D(consumed);
    }
    if (!EnsureInputs(s, in)) {
        return;
    }
    const u32 w = in.render_width, h = in.render_height;

    // Everything written before (depth, motion, the upscaled scene) is read by transfers.
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite,
    };
    std::array<vk::ImageMemoryBarrier2, 3> to_dst{};
    const vk::Image dst[] = {s.depth.image, s.motion.image, s.hudless.image};
    for (u32 i = 0; i < 3; ++i) {
        to_dst[i] = {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = dst[i],
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
    }
    const u32 images = in.hudless ? 3u : 2u;
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1,
                             .pMemoryBarriers = &before,
                             .imageMemoryBarrierCount = images,
                             .pImageMemoryBarriers = to_dst.data()});
    const vk::BufferImageCopy depth_copy{
        .imageSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
        .imageExtent = {w, h, 1},
    };
    cmdbuf.copyImageToBuffer(in.depth, vk::ImageLayout::eGeneral, s.staging, depth_copy);
    const vk::BufferMemoryBarrier2 staged{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .buffer = s.staging,
        .size = VK_WHOLE_SIZE,
    };
    cmdbuf.pipelineBarrier2({.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &staged});
    const vk::BufferImageCopy depth_store{
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {w, h, 1},
    };
    cmdbuf.copyBufferToImage(s.staging, s.depth.image, vk::ImageLayout::eTransferDstOptimal,
                             depth_store);
    const vk::ImageCopy motion_copy{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = {w, h, 1},
    };
    cmdbuf.copyImage(in.motion, vk::ImageLayout::eGeneral, s.motion.image,
                     vk::ImageLayout::eTransferDstOptimal, motion_copy);
    if (in.hudless) {
        const vk::ImageCopy hudless_copy{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .extent = {in.out_width, in.out_height, 1},
        };
        cmdbuf.copyImage(in.hudless, vk::ImageLayout::eGeneral, s.hudless.image,
                         vk::ImageLayout::eTransferDstOptimal, hudless_copy);
    }
    // To D3D12 in General; the sources may be written again after the copies.
    std::array<vk::ImageMemoryBarrier2, 3> to_general{};
    for (u32 i = 0; i < 3; ++i) {
        to_general[i] = {
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = dst[i],
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
    }
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1,
                             .pMemoryBarriers = &after,
                             .imageMemoryBarrierCount = images,
                             .pImageMemoryBarriers = to_general.data()});

    s.constants = MakeConstants(in);
    s.has_hudless = bool(in.hudless);
    s.consumed = 0;
    std::scoped_lock lock{g->latest_mutex};
    g->latest = int(j);
}

int TakeInputs() {
    if (!g) {
        return -1;
    }
    std::scoped_lock lock{g->latest_mutex};
    const int inputs = g->latest;
    g->latest = -1;
    if (inputs >= 0) {
        g->sets[inputs].consumed = kTaken;
    }
    return inputs;
}

} // namespace Vulkan::FrameGen

#endif
