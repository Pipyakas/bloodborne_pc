// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace BbSettings {

namespace {

const char* Path() {
    const char* env = std::getenv("BB_CONFIG");
    return env && env[0] ? env : "bbport.ini";
}

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    if (key == "upscaler") {
        // Before fsr4_model: upscaler=fsr411 was the DLL model.
        if (value == "fsr411") {
            v.upscaler = UpscalerFsr4;
            v.fsr4_model = Fsr4ModelDll;
        }
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.upscaler = u;
            }
        }
    } else if (key == "fsr4_model") {
        if (value == "auto") { // its name before highest
            v.fsr4_model = Fsr4ModelHighest;
        }
        for (int m = 0; m < Fsr4ModelCount; ++m) {
            if (value == Fsr4ModelName(m)) {
                v.fsr4_model = m;
            }
        }
    } else if (key == "preset") {
        v.preset = std::clamp(i, 0, PresetCount - 1);
    } else if (key == "dlss_model") {
        for (int m = 0; m < DlssModelCount; ++m) {
            if (value == DlssModels[m].key) {
                v.dlss_model = m;
            }
        }
    } else if (key == "frame_gen") {
        for (int m = 0; m < FrameGenCount; ++m) {
            if (value == FrameGenName(m)) {
                v.frame_gen = m;
            }
        }
    } else if (key == "sharpen") {
        v.sharpen = i != 0;
    } else if (key == "sharpness") {
        v.sharpness = Clamp(f, 0.0f, 2.0f);
    } else if (key == "jitter") {
        v.jitter = i != 0;
    } else if (key == "reactive") {
        v.reactive = i != 0;
    } else if (key == "object_motion") {
        v.object_motion = i != 0;
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "debug_view") {
        v.debug_view = std::clamp(i, 0, DebugViewCount - 1);
    } else if (key == "show_fps") {
        v.show_fps = i != 0;
    } else if (key == "fsr4_auto_exposure") {
        v.fsr4_auto_exposure = i != 0;
    } else if (key == "fsr4_invert_jitter") {
        v.fsr4_invert_jitter = i != 0;
    } else if (key == "model_lod") {
        v.model_lod = std::clamp(i, -2, 2);
    } else if (key == "fullscreen") {
        v.fullscreen = i != 0;
    } else if (key == "frame_limit") {
        v.frame_limit = std::clamp(i, 0, 120);
    } else if (key == "render_scale") {
        v.render_scale = std::clamp((i + RenderScaleStep / 2) / RenderScaleStep * RenderScaleStep,
                                    RenderScaleMin, RenderScaleMax);
    } else if (key == "dynamic_resolution") {
        v.dynamic_resolution = i != 0;
    } else if (key == "maximized") {
        v.maximized = i != 0;
    } else if (key == "background_gamepad") {
        v.background_gamepad = i != 0;
    } else if (key == "keyboard_controls") {
        v.keyboard_controls = i != 0;
    } else if (key == "overlay_docked") {
        v.overlay_docked = i != 0;
    } else if (key == "hide_cursor") {
        v.hide_cursor = i != 0;
    } else if (key == "mute") {
        v.mute = i != 0;
    } else if (key == "mute_background") {
        v.mute_background = i != 0;
    } else if (key == "launch") {
        for (int l = 0; l < LaunchCount; ++l) {
            if (value == LaunchName(l)) {
                v.launch = l;
            }
        }
    } else if (key == "live_resolution") {
        v.live_resolution = value == "auto" ? -1 : std::clamp(i, 0, 1);
    } else if (key == "output_res") {
        for (int r = 0; r < OutputCount; ++r) {
            if (value == std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r])) {
                v.output_res = r;
            }
        }
    } else {
        for (int e = 0; e < EffectCount; ++e) {
            if (key == Effects[e].key) {
                v.effects[e] = i != 0;
            }
        }
    }
}

} // namespace

Values& Get() {
    static Values values;
    return values;
}

void Load() {
    auto& v = Get();
    for (int e = 0; e < EffectCount; ++e) {
        v.effects[e] = Effects[e].default_on;
    }
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", Path());
    }
    v.launch_saved = v.launch.load();
    // Environment overrides (scripts, A/B tests).
    if (const char* env = std::getenv("BB_UPSCALER")) {
        v.upscaler = UpscalerOff;
        Set(v, "upscaler", env);
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"BB_FSR_SHARPNESS", "sharpness"},        {"BB_JITTER", "jitter"},
        {"BB_REACTIVE", "reactive"},              {"BB_REACTIVE_SCALE", "reactive_scale"},
        {"BB_REACTIVE_THRESHOLD", "reactive_threshold"}, {"BB_REACTIVE_MAX", "reactive_max"},
        {"BB_UPSCALE_PRESET", "preset"},            {"BB_OBJECT_MOTION", "object_motion"},
        {"BB_LAUNCH", "launch"},
        {"BB_DLSS_MODEL", "dlss_model"},            {"BB_FRAME_GEN", "frame_gen"},
        {"BB_FSR4_MODEL", "fsr4_model"},
        {"BB_RENDER_SCALE", "render_scale"},        {"BB_DYNAMIC_RES", "dynamic_resolution"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env)) {
            Set(v, key, value);
        }
    }
    v.startup_preset = v.preset;
    v.startup_upscaler = v.upscaler;
    v.startup_object_motion = v.object_motion;
    for (int e = 0; e < EffectCount; ++e) {
        v.startup_effects[e] = v.effects[e];
    }
    v.startup_model_lod = v.model_lod;
    v.startup_output_res = v.output_res;
    v.startup_live_resolution = v.live_resolution;
    v.startup_frame_gen = v.frame_gen;
}

void ConfigureUpscalerSupport(bool fsr4, bool fsr4_dll, bool dlss) {
    auto& v = Get();
    v.fsr4_supported = fsr4;
    v.fsr4_dll_supported = fsr4 && fsr4_dll;
    v.dlss_supported = dlss;
    const int requested = v.upscaler;
    if (requested == UpscalerDlss && !dlss) {
        v.fsr4_problem = "DLSS needs an NVIDIA RTX GPU, its driver's NGX and nvngx_dlss; using FSR 3.1";
        std::printf("Upscaler: dlss unavailable; falling back to FSR 3.1 before the first frame\n");
        v.upscaler = UpscalerFsr3;
    } else if (requested == UpscalerFsr4 && !v.fsr4_supported) {
        v.fsr4_problem = "GPU does not support the FSR 4 shaders; using FSR 3.1";
        std::printf("Upscaler: %s unsupported on this GPU; falling back to FSR 3.1 before the first frame\n",
                    UpscalerName(requested));
        v.upscaler = UpscalerFsr3;
    }
}

bool FixedRenderSession() {
    const char* size = std::getenv("BB_RENDER_RES");
    return size && size[0];
}

int RenderPreset() {
    const auto& v = Get();
    return FixedRenderSession() ? v.startup_preset :
        v.upscaler == UpscalerTaa ? NativeAA : v.preset.load();
}

bool ResolutionNeedsRestart() {
    const auto& v = Get();
    // TAA needs the live path (native guest targets): run.sh selects it on restart.
    return FixedRenderSession() &&
        (v.preset != v.startup_preset || v.output_res != v.startup_output_res ||
         (v.upscaler == UpscalerOff) != (v.startup_upscaler == UpscalerOff) ||
         (v.upscaler == UpscalerTaa) != (v.startup_upscaler == UpscalerTaa));
}

void Save() {
    const auto& v = Get();
    FILE* file = std::fopen(Path(), "w");
    if (!file) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    std::fprintf(file,
                 "# bbport settings (in-game menu: F1 / L3+R3)\n"
                 "upscaler=%s\npreset=%d\ndlss_model=%s\nframe_gen=%s\nsharpen=%d\nsharpness=%.2f\njitter=%d\n"
                 "reactive=%d\nobject_motion=%d\nreactive_scale=%.2f\nreactive_threshold=%.2f\nreactive_max=%.2f\n"
                 "debug_view=%d\nshow_fps=%d\nfsr4_auto_exposure=%d\nfsr4_invert_jitter=%d\nfsr4_model=%s\n",
                 UpscalerName(v.upscaler), v.preset.load(), DlssModels[v.dlss_model].key,
                 FrameGenName(v.frame_gen),
                 int(v.sharpen.load()),
                 v.sharpness.load(), int(v.jitter.load()), int(v.reactive.load()),
                 int(v.object_motion.load()),
                 v.reactive_scale.load(), v.reactive_threshold.load(), v.reactive_max.load(),
                 v.debug_view.load(), int(v.show_fps.load()),
                 int(v.fsr4_auto_exposure.load()), int(v.fsr4_invert_jitter.load()),
                 Fsr4ModelName(v.fsr4_model));
    // Read by patches.py at start.
    for (int e = 0; e < EffectCount; ++e) {
        std::fprintf(file, "%s=%d\n", Effects[e].key, int(v.effects[e].load()));
    }
    std::fprintf(file, "model_lod=%d\noutput_res=%dx%d\nfullscreen=%d\n", v.model_lod.load(),
                 OutputWidths[v.output_res], OutputHeights[v.output_res], int(v.fullscreen.load()));
    std::fprintf(file,
                 "maximized=%d\nframe_limit=%d\nrender_scale=%d\ndynamic_resolution=%d\n"
                 "background_gamepad=%d\nhide_cursor=%d\nmute=%d\nmute_background=%d\n"
                 "launch=%s\nkeyboard_controls=%d\noverlay_docked=%d\n",
                 int(v.maximized.load()), v.frame_limit.load(), v.render_scale.load(),
                 int(v.dynamic_resolution.load()), int(v.background_gamepad.load()),
                 int(v.hide_cursor.load()), int(v.mute.load()), int(v.mute_background.load()),
                 LaunchName(v.launch_saved), int(v.keyboard_controls.load()),
                 int(v.overlay_docked.load()));
    // Read by run.sh at start.
    std::fprintf(file, "live_resolution=%s\n", v.live_resolution < 0 ? "auto"
                                                  : v.live_resolution ? "1" : "0");
    std::fclose(file);
}

float PresetScale(int preset) {
    static constexpr float scales[PresetCount] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return scales[std::clamp(preset, 0, PresetCount - 1)];
}

const char* PresetName(int preset) {
    static constexpr const char* names[PresetCount] = {"Native AA", "Quality", "Balanced",
                                                       "Performance", "Ultra Performance"};
    return names[std::clamp(preset, 0, PresetCount - 1)];
}

const char* FrameGenName(int mode) {
    static constexpr const char* names[FrameGenCount] = {"off", "2x", "3x", "4x", "dynamic"};
    return names[std::clamp(mode, 0, FrameGenCount - 1)];
}

const char* FrameGenLabel(int mode) {
    static constexpr const char* labels[FrameGenCount] = {
        "Off", "2x (1 generated frame)", "3x (2 generated frames)", "4x (3 generated frames)",
        "Dynamic (to the display refresh rate)"};
    return labels[std::clamp(mode, 0, FrameGenCount - 1)];
}

int DlssAutoPreset(int preset) {
    return preset == Performance ? 13 : preset == UltraPerformance ? 12 : 11;
}

const char* DlssGeneration(int ngx_preset) {
    return ngx_preset == 5 || ngx_preset == 6 ? "CNN (DLSS 3)"
         : ngx_preset == 12 || ngx_preset == 13 ? "Transformer 2 (DLSS 4.5)"
                                                : "Transformer (DLSS 4)";
}

const char* UpscalerName(int upscaler) {
    static constexpr const char* names[UpscalerCount] = {"off", "fsr3", "fsr4", "taa", "dlss"};
    return names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

const char* Fsr4ModelName(int model) {
    static constexpr const char* names[Fsr4ModelCount] = {"highest", "dll", "sdk"};
    return names[std::clamp(model, 0, Fsr4ModelCount - 1)];
}

int CompareVersions(const char* a, const char* b) {
    for (;;) {
        char* end_a = nullptr;
        char* end_b = nullptr;
        const long x = a && *a ? std::strtol(a, &end_a, 10) : 0;
        const long y = b && *b ? std::strtol(b, &end_b, 10) : 0;
        if (x != y) {
            return x < y ? -1 : 1;
        }
        a = end_a && *end_a == '.' ? end_a + 1 : nullptr;
        b = end_b && *end_b == '.' ? end_b + 1 : nullptr;
        if (!a && !b) {
            return 0;
        }
    }
}

int Fsr4PreferredModel() {
    const auto& v = Get();
    const int choice = v.fsr4_model;
    if (choice != Fsr4ModelHighest) {
        return choice;
    }
    const char* dll = v.fsr4_dll_version.load();
    if (!dll || !v.fsr4_dll_supported) {
        return Fsr4ModelSdk;
    }
    // A DLL set from before manifests: its replay only exists for the 4.1 models.
    return CompareVersions(dll[0] ? dll : "4.1", v.fsr4_sdk_version.load()) > 0 ? Fsr4ModelDll
                                                                               : Fsr4ModelSdk;
}

const char* LaunchName(int launch) {
    static constexpr const char* names[LaunchCount] = {"title", "offline", "continue", "load",
                                                       "new_game", "system"};
    return names[std::clamp(launch, 0, LaunchCount - 1)];
}

const char* LaunchLabel(int launch) {
    static constexpr const char* labels[LaunchCount] = {
        "Title screen", "Play Offline menu", "Continue the last save", "Load Game",
        "New Game", "System settings"};
    return labels[std::clamp(launch, 0, LaunchCount - 1)];
}

} // namespace BbSettings

// bbport: the control channel's "set <key> <value>" (src/runtime_control.c): one bbport.ini
// setting applied while the game runs, as the in-game menus change it (not saved).
extern "C" void bbgpu_set_setting(const char* key, const char* value) {
    BbSettings::Set(BbSettings::Get(), key, value);
}
