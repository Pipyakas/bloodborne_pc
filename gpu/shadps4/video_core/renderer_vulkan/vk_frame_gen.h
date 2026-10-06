// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: DLSS Frame Generation (NVIDIA Streamline 2.x) for the Vulkan renderer, Windows only.
//
// Streamline's frame generation hooks a D3D12/DXGI swapchain, so with frame_gen on (bbport.ini,
// read at start) the window is presented through DXGI instead of a Vulkan swapchain: the
// presenter draws into Vulkan images that alias D3D12 textures (external memory), a D3D12 fence
// shared as a Vulkan timeline semaphore orders the two APIs, and the D3D12 queue copies the
// image into the DXGI back buffer and presents it; Streamline inserts the generated frames.
// Its inputs come from the upscaler (TemporalUpscaler::RunScaled): scene depth and motion
// vectors at render size and the upscaled scene before the UI (hud-less color), copied into
// shared textures and tagged for the flip of that frame, with the camera matrices.
//
// sl.interposer.dll and the plugins are searched in out/streamline (tools/fetch_streamline.sh;
// BB_STREAMLINE_DIR overrides). RTX 20/30 GPUs need the dlssg_sm86 mod for DLSS-G: its proxy
// DLL (out/dlssg_sm86/version.dll with dlssg_sm86.ini, BB_DLSSG_MOD names another file, "0"
// skips it) is loaded before Streamline. Anything missing leaves the Vulkan swapchain in use.

#pragma once

#include <array>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
struct SubmitInfo;

namespace FrameGen {

/// Scene camera in Bloodborne's conventions (vk_camera_motion.h): 3x4 rows, view z forward.
struct Camera {
    std::array<float, 12> view, inv_view, prev_view, prev_inv_view;
    std::array<float, 4> proj, prev_proj;
};

/// One rendered frame's inputs, all in General layout when recorded.
struct Inputs {
    vk::Image depth;   ///< D32 (+S8) scene depth, render size
    vk::Image motion;  ///< RG16F, render pixels, previous - current, without jitter
    vk::Image hudless; ///< RGBA8 upscaled scene before the UI, output size
    u32 render_width, render_height, out_width, out_height;
    std::array<float, 2> jitter; ///< render pixels
    Camera camera;
    bool reset;
};

/// Sets frame generation up when bbport.ini asks for it at start (Swapchain constructor).
/// False: off, not Windows, or something missing (logged, and shown in the menu).
bool Init(const Instance& instance, void* hwnd);
/// Set up and presenting through DXGI.
bool Active();
void Shutdown();

// The Swapchain in DXGI mode.
void Resize(u32 width, u32 height);
u32 ImageCount();
vk::Format Format();
vk::Image Image(u32 index);
vk::ImageView View(u32 index);
/// The next image to draw into; waits until D3D12 has copied it out.
u32 Acquire();
/// The presenter's submission signals the shared fence for image `index`.
void AddPresentSignal(SubmitInfo& info, u32 index);
/// Presents image `index` with the inputs taken at its flip (TakeInputs, -1 for none).
bool Present(u32 index, int inputs);

/// Upscaler, GPU thread: copies this frame's inputs into the next set of shared textures.
void RecordInputs(vk::CommandBuffer cmdbuf, const Inputs& inputs);
/// Presenter, GPU thread at the flip: the input set recorded for this frame, or -1.
int TakeInputs();
/// Presenter: a frame taken with `inputs` is not presented (minimised window, skipped
/// frame); its input set may be recorded again at once.
void DropInputs(int inputs);

} // namespace FrameGen
} // namespace Vulkan
