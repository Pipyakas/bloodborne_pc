// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: user settings changed at run time from the in-game menu (bbport_overlay.h) and kept
// in bbport.ini (BB_CONFIG overrides the path). Environment variables override the file at
// start. Readers load the atomics every frame; writers are the menu and Load().

#pragma once

#include <atomic>

namespace BbSettings {

enum Upscaler : int { UpscalerOff = 0, UpscalerFsr3 = 1, UpscalerFsr4 = 2, UpscalerFsr411 = 3,
                      UpscalerTaa = 4, UpscalerDlss = 5, UpscalerCount };
/// FSR 4 v07 or FSR 4.1.1: the same inputs, settings and placement in the frame.
inline bool IsFsr4(int upscaler) {
    return upscaler == UpscalerFsr4 || upscaler == UpscalerFsr411;
}
/// FSR 4, FSR 4.1.1 or DLSS: one frame's inputs to a separate upscaler, which writes the
/// output image (TemporalUpscaler::RecordFsr4); FSR 3.1 and TAA are recorded in place.
inline bool IsFrameUpscaler(int upscaler) {
    return IsFsr4(upscaler) || upscaler == UpscalerDlss;
}
enum Preset : int { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, PresetCount };
/// DLSS Super Resolution model (NGX render preset), by generation. Auto lets the driver pick
/// per quality mode (310.9: K for DLAA/Quality/Balanced, M for Performance, L for Ultra
/// Performance; an NVIDIA App override may change it).
struct DlssModel {
    const char* key;    ///< bbport.ini dlss_model value
    const char* label;  ///< menu
    int ngx_preset;     ///< NVSDK_NGX_DLSS_Hint_Render_Preset (0 = default)
};
inline constexpr DlssModel DlssModels[] = {
    {"auto", "Auto (driver default per mode)", 0},
    {"e", "DLSS 3 - CNN (preset E)", 5},
    {"j", "DLSS 4 - Transformer (preset J)", 10},
    {"k", "DLSS 4 - Transformer (preset K)", 11},
    {"m", "DLSS 4.5 - Transformer 2 (preset M)", 13},
    {"l", "DLSS 4.5 - Transformer 2 (preset L)", 12},
};
inline constexpr int DlssModelCount = int(sizeof(DlssModels) / sizeof(DlssModels[0]));
/// DLSS Frame Generation (Streamline, Windows): generated frames per rendered frame, or
/// NVIDIA's dynamic multi-frame generation toward the display refresh rate.
enum FrameGen : int { FrameGenOff = 0, FrameGen2x, FrameGen3x, FrameGen4x, FrameGenDynamic,
                      FrameGenCount };
const char* FrameGenName(int mode);  ///< bbport.ini: off, 2x, 3x, 4x, dynamic
const char* FrameGenLabel(int mode); ///< menu
/// The NGX preset Auto resolves to for a bbport preset (DLSS 310.9 defaults).
int DlssAutoPreset(int preset);
/// "CNN (DLSS 3)", "Transformer (DLSS 4)" or "Transformer 2 (DLSS 4.5)" for an NGX preset.
const char* DlssGeneration(int ngx_preset);
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugViewCount };
/// Launch shortcut (runtime_menu.c): where the game goes at start, as if the title screen's
/// rows were selected. Applied once per start; without a save Continue stops at the menu.
enum Launch : int { LaunchTitle = 0, LaunchOffline, LaunchContinue, LaunchLoad, LaunchNewGame,
                    LaunchSystem, LaunchCount };

/// Game effects switched by the community patches at start (patches.py EFFECTS): ini key,
/// menu label, default (the game's own behaviour).
struct Effect {
    const char* key;
    const char* label;
    bool default_on;
};
inline constexpr Effect Effects[] = {
    {"effect_chromatic_aberration", "Chromatic aberration", true},
    {"effect_dof", "Depth of field (DoF)", true},
    {"effect_motion_blur", "Motion blur", true},
    {"effect_ssao", "SSAO ambient occlusion", true},
    {"effect_game_aa", "The game's own anti-aliasing", true},
    {"effect_dynamic_shadows", "Shadows from dynamic lights", true},
    {"effect_ssr", "SSR reflections (not in the original game)", false},
    {"skip_intro", "Skip intro videos at startup", false},
    {"debug_camera", "Free camera (Cross + L3)", false},
    {"debug_menu", "Debug menu (needs font files)", false},
};
inline constexpr int EffectCount = int(sizeof(Effects) / sizeof(Effects[0]));
/// Live output resolutions: the upscaler's output and the UI host targets.
inline constexpr int OutputWidths[] = {1280, 1920, 2560, 3840};
inline constexpr int OutputHeights[] = {720, 1080, 1440, 2160};
inline constexpr int OutputCount = 4;
inline constexpr int OutputDefault = 2; ///< 2560x1440 (index 1, 1920x1080, is the game's own size)
/// Frame rate limits (frame_limit, FPS; 0: the display refresh rate). The uncapped frame rate
/// preset keeps the game's timing up to 120 FPS.
inline constexpr int FrameLimits[] = {0, 30, 60, 90, 120};
inline constexpr int FrameLimitCount = 5;
/// render_scale: 5..200% in steps of 5.
inline constexpr int RenderScaleMin = 5, RenderScaleMax = 200, RenderScaleStep = 5;

struct Values {
    // Defaults: DLSS (FSR 3.1 where it is unavailable), Performance, CNN model, 4x generation.
    std::atomic<int> upscaler{UpscalerDlss};
    std::atomic<int> preset{Performance};
    std::atomic<bool> sharpen{true};
    std::atomic<float> sharpness{0.3f};
    std::atomic<bool> jitter{true};
    std::atomic<bool> reactive{false};
    std::atomic<bool> object_motion{true};
    std::atomic<float> reactive_scale{1.0f};
    std::atomic<float> reactive_threshold{0.2f};
    std::atomic<float> reactive_max{0.9f};
    std::atomic<int> dlss_model{1}; ///< index into DlssModels (1: CNN, preset E)
    /// Frame generation. Off <-> on is applied on restart (the presentation path is chosen at
    /// start); between the on modes it changes live.
    std::atomic<int> frame_gen{FrameGen4x};
    std::atomic<int> debug_view{DebugNone};
    std::atomic<bool> show_fps{false};
    // FSR 4 checks (menu): the provider's auto exposure, the jitter sign it is given.
    std::atomic<bool> fsr4_auto_exposure{true};
    std::atomic<bool> fsr4_invert_jitter{false};
    std::atomic<int> active_render_width{1920}, active_render_height{1080};
    /// The temporal upscaler ran on the last frame (menus and loading screens skip it).
    std::atomic<bool> upscaler_ran{false};
    /// Applied at start (patches.py); the menu shows when a restart is needed.
    std::atomic<bool> effects[EffectCount]{};
    std::atomic<int> model_lod{0}; ///< -2 highest .. 2 lowest, 0 the game's
    std::atomic<int> output_res{OutputDefault}; ///< index into OutputWidths
    /// Borderless fullscreen window at the desktop size (F11 toggles; BB_FULLSCREEN overrides).
    std::atomic<bool> fullscreen{false};
    /// Live resolution and preset changes (run.sh): -1 auto by default (on for strong discrete
    /// GPUs: presets switch without a restart, ~15% more GPU time), 0 off (startup patch,
    /// fastest; the Steam Deck and older GPUs), 1 on. On restart.
    std::atomic<int> live_resolution{-1};
    /// Window and input (on start): a maximised window; gamepads read while unfocused.
    std::atomic<bool> maximized{true};
    std::atomic<int> frame_limit{0}; ///< FPS, 0: display refresh rate (at most 120)
    /// Render resolution in percent of the output for the modes without presets (upscaler off,
    /// TAA): 5..200 in steps of 5; above 100 supersamples.
    std::atomic<int> render_scale{100};
    /// Dynamic resolution, replacing the preset (or a render_scale below 100): while the GPU
    /// misses the frame rate target, the render resolution drops in 5% steps until the CPU
    /// limits the frame rate (or a step saves no GPU time); it rises again while the GPU has
    /// headroom, up to 100% of the output (render_scale above 100: that).
    std::atomic<bool> dynamic_resolution{false};
    /// Set by the renderer: the dynamic resolution in percent of the output (0: not active),
    /// and the GPU's busy time per rendered frame (ms, 0 before the first measurement).
    std::atomic<int> dynamic_percent{0};
    std::atomic<float> gpu_frame_ms{0.0f};
    std::atomic<bool> background_gamepad{true};
    /// Mouse cursor hidden after 500 ms without motion, or at once on gamepad input.
    std::atomic<bool> hide_cursor{true};
    /// The keyboard plays the game (runtime_pad.c); off leaves the keys to other programs.
    std::atomic<bool> keyboard_controls{true};
    /// The settings menu opens docked over the whole window (drag its tab to undock).
    std::atomic<bool> overlay_docked{true};
    /// Audio: everything muted, or muted while the window is not focused.
    std::atomic<bool> mute{false};
    std::atomic<bool> mute_background{true};
    /// Launch shortcut (BB_LAUNCH, --launch override it for one start). Read at start.
    std::atomic<int> launch{LaunchContinue};
    /// The file's choice, written back by Save (an override is not saved).
    std::atomic<int> launch_saved{LaunchContinue};
    /// Set by the window thread.
    std::atomic<bool> window_focused{true};
    /// Why FSR 4 cannot run (assets, device features), or null. Set by the renderer.
    std::atomic<const char*> fsr4_problem{nullptr};
    std::atomic<bool> fsr4_supported{false}, fsr411_supported{false}, dlss_supported{false};
    /// NGX preset of the DLSS feature in use (Auto resolved), 0 before the first DLSS frame.
    std::atomic<int> dlss_active_preset{0};
    /// Frame generation state for the menu: set up at start, generating now, why not (or null).
    std::atomic<bool> frame_gen_ready{false}, frame_gen_active{false};
    std::atomic<int> frame_gen_presented{1}; ///< frames shown per rendered frame (DLSS-G state)
    std::atomic<const char*> frame_gen_problem{nullptr};

    /// Startup settings for the explicit BB_RENDER_RES compatibility patch only.
    int startup_preset = Performance;
    int startup_upscaler = UpscalerDlss;
    bool startup_object_motion = true;
    bool startup_effects[EffectCount]{};
    /// Effects runtime_effects.c switches while the game runs (startup_effects follows them).
    std::atomic<bool> live_effects[EffectCount]{};
    int startup_model_lod = 0;
    int startup_output_res = OutputDefault;
    int startup_live_resolution = -1;
    int startup_frame_gen = FrameGen4x;
};

Values& Get();

/// Reads the file, then the environment overrides. Called once at start.
void Load();
/// Checks the loaded choice before the first frame; unsupported FSR 4 or DLSS uses FSR 3.1.
void ConfigureUpscalerSupport(bool fsr4, bool fsr411, bool dlss);
/// Startup-patched scene dimensions cannot change until run.sh prepares a new image.
bool FixedRenderSession();
int RenderPreset();
bool ResolutionNeedsRestart();
/// Writes the file (menu changes).
void Save();

/// Render resolution divisor of a preset (1.0 native, 1.5 quality, ...).
float PresetScale(int preset);
const char* PresetName(int preset);
const char* UpscalerName(int upscaler);
/// bbport.ini / --launch names: title, offline, continue, load, new_game, system.
const char* LaunchName(int launch);
/// Menu label of a launch shortcut.
const char* LaunchLabel(int launch);

} // namespace BbSettings
