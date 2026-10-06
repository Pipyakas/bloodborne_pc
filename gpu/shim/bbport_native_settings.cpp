// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the port's settings as rows of the game's own options screen (runtime_menu.c adds
// the "Graphics" screen). The game's widgets edit one byte per row; the window thread
// (Poll) turns changed bytes into settings and saves them.
#include "bbport_native_settings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>

#include "../bbgpu.h"
#include "bbport_settings.h"

namespace BbNative {

namespace {

using BbSettings::Values;

enum Kind : int32_t { Toggle = BB_NATIVE_TOGGLE, Choice = BB_NATIVE_CHOICE, Slider = BB_NATIVE_SLIDER };

/// One row: how the byte is read from and written to the settings.
struct Row {
    int screen; ///< BB_NATIVE_SCREEN, BB_NATIVE_ADVANCED or BB_NATIVE_UPSCALING
    const char16_t* label;
    const char16_t* help;
    Kind kind;
    int (*get)(const Values&);
    void (*set)(Values&, int);
    int default_value;
    /// A choice row's entries: their count, *labels set.
    int (*choices)(const char16_t* const** labels) = nullptr;
    /// Whether the row is shown (null: always), read when its screen opens.
    bool (*shown)(const Values&) = nullptr;
};

int EffectIndex(const char* key) {
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        if (std::string_view{BbSettings::Effects[e].key} == key) return e;
    }
    return 0;
}
template <const char* Key>
int GetEffect(const Values& v) {
    return v.effects[EffectIndex(Key)] ? 1 : 0;
}
template <const char* Key>
void SetEffect(Values& v, int on) {
    v.effects[EffectIndex(Key)] = on != 0;
}
constexpr char kMotionBlur[] = "effect_motion_blur";
constexpr char kDof[] = "effect_dof";
constexpr char kChromatic[] = "effect_chromatic_aberration";
constexpr char kSsao[] = "effect_ssao";
constexpr char kSsr[] = "effect_ssr";

// The upscaler choices offered: the ones this GPU runs (the renderer checks them at start).
std::array<int, BbSettings::UpscalerCount> upscalers{};
int upscaler_count = 0;
const char16_t* const UpscalerLabels[BbSettings::UpscalerCount] = {
    u"Off", u"FSR 3.1", u"FSR 4", u"FSR 4.1.1", u"TAA", u"DLSS"};
std::array<const char16_t*, BbSettings::UpscalerCount> upscaler_labels{};

const char16_t* const PresetLabels[] = {u"Native", u"Quality", u"Balanced", u"Performance",
                                        u"Ultra Performance"};
const char16_t* const OutputLabels[] = {u"1280 x 720", u"1920 x 1080", u"2560 x 1440",
                                        u"3840 x 2160"};
// Screen mode: the "fullscreen" and "maximized" settings (fullscreen wins).
const char16_t* const ScreenModeLabels[] = {u"Windowed", u"Maximised window", u"Full screen"};
const char16_t* const FrameLimitLabels[BbSettings::FrameLimitCount] = {
    u"Display refresh rate", u"30 FPS", u"60 FPS", u"90 FPS", u"120 FPS"};
// BbSettings::DlssModels and FrameGen, in their order.
const char16_t* const DlssModelLabels[] = {u"Auto", u"DLSS 3 CNN (E)", u"DLSS 4 Transformer (J)",
                                           u"DLSS 4 Transformer (K)", u"DLSS 4.5 Transformer 2 (M)",
                                           u"DLSS 4.5 Transformer 2 (L)"};
static_assert(std::size(DlssModelLabels) == BbSettings::DlssModelCount);
const char16_t* const FrameGenLabels[] = {u"Off", u"2x", u"3x (multi frame)", u"4x (multi frame)",
                                          u"Dynamic (multi frame)"};
static_assert(std::size(FrameGenLabels) == BbSettings::FrameGenCount);

// The Upscaler row's default: the entry of the settings' default (DLSS, else FSR 3.1).
constexpr int UpscalerDefault = -1;

template <size_t N>
int Labels(const char16_t* const (&list)[N], const char16_t* const** out) {
    *out = list;
    return int(N);
}

// Each screen lists its choice rows first (runtime_menu.c finds their dropdowns in order).
const Row rows[] = {
    // Screen
    {BB_NATIVE_SCREEN, u"Screen mode", u"Window, maximised window or borderless full screen (F11).",
     Choice, [](const Values& v) { return v.fullscreen ? 2 : v.maximized ? 1 : 0; },
     [](Values& v, int i) {
         v.fullscreen = i == 2;
         if (i < 2) v.maximized = i == 1;
     },
     1, [](const char16_t* const** out) { return Labels(ScreenModeLabels, out); }},
    {BB_NATIVE_SCREEN, u"Resolution", u"Resolution of the upscaled image and the interface.", Choice,
     [](const Values& v) { return v.output_res.load(); },
     [](Values& v, int i) { v.output_res = std::clamp(i, 0, BbSettings::OutputCount - 1); },
     BbSettings::OutputDefault, [](const char16_t* const** out) { return Labels(OutputLabels, out); }},
    {BB_NATIVE_SCREEN, u"Frame rate limit", u"Caps real rendered frames (up to 120 FPS). Frame generation adds frames on top.",
     Choice,
     [](const Values& v) {
         for (int i = 0; i < BbSettings::FrameLimitCount; ++i) {
             if (BbSettings::FrameLimits[i] == v.frame_limit) return i;
         }
         return 0;
     },
     [](Values& v, int i) {
         v.frame_limit = BbSettings::FrameLimits[std::clamp(i, 0, BbSettings::FrameLimitCount - 1)];
     },
     0, [](const char16_t* const** out) { return Labels(FrameLimitLabels, out); }},
    {BB_NATIVE_SCREEN, u"Frame rate counter", u"Frame rate and frame time in a corner of the screen.",
     Toggle, [](const Values& v) { return v.show_fps ? 1 : 0; },
     [](Values& v, int on) { v.show_fps = on != 0; }, 0},
    // Advanced options
    {BB_NATIVE_ADVANCED, u"Motion blur", u"Camera and object motion blur.",
     Toggle, GetEffect<kMotionBlur>, SetEffect<kMotionBlur>, 1},
    {BB_NATIVE_ADVANCED, u"Depth of field", u"Background blur.", Toggle,
     GetEffect<kDof>, SetEffect<kDof>, 1},
    {BB_NATIVE_ADVANCED, u"Chromatic aberration", u"Colour fringes at the screen edges.",
     Toggle, GetEffect<kChromatic>, SetEffect<kChromatic>, 1},
    {BB_NATIVE_ADVANCED, u"Ambient occlusion", u"Contact shadows (SSAO).", Toggle,
     GetEffect<kSsao>, SetEffect<kSsao>, 1},
    {BB_NATIVE_ADVANCED, u"Screen space reflections", u"Reflections not in the original game. Applied after restarting the game.",
     Toggle, GetEffect<kSsr>, SetEffect<kSsr>, 0},
    // Upscaling
    {BB_NATIVE_UPSCALING, u"Upscaler", u"Temporal upscaler or anti-aliasing for the 3D scene.", Choice,
     [](const Values& v) {
         for (int i = 0; i < upscaler_count; ++i) {
             if (upscalers[i] == v.upscaler) return i;
         }
         return 0;
     },
     [](Values& v, int i) { v.upscaler = upscalers[std::clamp(i, 0, upscaler_count - 1)]; },
     UpscalerDefault,
     [](const char16_t* const** out) {
         *out = upscaler_labels.data();
         return upscaler_count;
     }},
    {BB_NATIVE_UPSCALING, u"Upscaling quality", u"Render resolution: Native renders at the output resolution.",
     Choice, [](const Values& v) { return v.preset.load(); },
     [](Values& v, int i) { v.preset = std::clamp(i, 0, BbSettings::PresetCount - 1); },
     BbSettings::Performance, [](const char16_t* const** out) { return Labels(PresetLabels, out); }},
    {BB_NATIVE_UPSCALING, u"DLSS model", u"DLSS 3 CNN is the lightest; the transformer models are sharper and heavier.",
     Choice, [](const Values& v) { return v.dlss_model.load(); },
     [](Values& v, int i) { v.dlss_model = std::clamp(i, 0, BbSettings::DlssModelCount - 1); }, 1,
     [](const char16_t* const** out) { return Labels(DlssModelLabels, out); },
     [](const Values& v) { return v.dlss_supported.load(); }},
#ifdef _WIN32
    {BB_NATIVE_UPSCALING, u"Frame generation",
     u"DLSS frames generated per rendered frame. Switching it on or off applies after a restart.",
     Choice, [](const Values& v) { return v.frame_gen.load(); },
     [](Values& v, int i) { v.frame_gen = std::clamp(i, 0, BbSettings::FrameGenCount - 1); },
     BbSettings::FrameGen4x, [](const char16_t* const** out) { return Labels(FrameGenLabels, out); },
     [](const Values& v) { return v.dlss_supported.load(); }},
#endif
    {BB_NATIVE_UPSCALING, u"Sharpness", u"Contrast-adaptive sharpening after upscaling (0: off).", Slider,
     [](const Values& v) {
         return v.sharpen ? int(std::lround(std::clamp(v.sharpness.load(), 0.0f, 1.0f) * 10)) : 0;
     },
     [](Values& v, int s) {
         v.sharpen = s > 0;
         if (s > 0) v.sharpness = float(std::clamp(s, 0, 10)) / 10.0f;
     },
     3},
};
constexpr int RowCount = int(sizeof(rows) / sizeof(rows[0]));

std::mutex mutex;

std::string Ascii(const char16_t* text) {
    std::string out;
    for (; *text; ++text) out += *text < 128 ? char(*text) : '?';
    return out;
}
std::array<BbNativeSetting, RowCount> table{};
// One aligned int32 per row: the game's choice rows write all four bytes.
std::array<int32_t, RowCount> values{}, applied{}, defaults{};
bool open_once = false;
// The game's "dropdown open" bytes of the open screen's choice rows (guest memory).
std::array<const volatile uint8_t*, RowCount> dropdown_open{};
// The open screen's rows (copies of table entries) and their places in rows[].
std::array<BbNativeSetting, RowCount> screen_rows{};
std::array<int, RowCount> screen_row_index{};

void BuildChoices() {
    const auto& v = BbSettings::Get();
    upscaler_count = 0;
    for (int u = 0; u < BbSettings::UpscalerCount; ++u) {
        const bool supported = (u != BbSettings::UpscalerFsr4 || v.fsr4_supported) &&
                               (u != BbSettings::UpscalerFsr411 || v.fsr411_supported) &&
                               (u != BbSettings::UpscalerDlss || v.dlss_supported);
        if (supported) {
            upscalers[upscaler_count] = u;
            upscaler_labels[upscaler_count] = UpscalerLabels[u];
            ++upscaler_count;
        }
    }
}

} // namespace

void Apply(bool choices);

int Rows(int screen, const BbNativeSetting** out) {
    Apply(true); // what a previous opening left pending
    std::scoped_lock lock{mutex};
    dropdown_open.fill(nullptr);
    BuildChoices();
    const auto& v = BbSettings::Get();
    for (int r = 0; r < RowCount; ++r) {
        values[r] = applied[r] = rows[r].get(v);
        defaults[r] = rows[r].default_value;
        if (defaults[r] == UpscalerDefault) {
            const int wanted = v.dlss_supported ? BbSettings::UpscalerDlss : BbSettings::UpscalerFsr3;
            defaults[r] = 0;
            for (int i = 0; i < upscaler_count; ++i) {
                if (upscalers[i] == wanted) defaults[r] = i;
            }
        }
        auto& t = table[r];
        t.label = reinterpret_cast<const uint16_t*>(rows[r].label);
        t.help = reinterpret_cast<const uint16_t*>(rows[r].help);
        t.kind = rows[r].kind;
        t.value = &values[r];
        t.default_value = &defaults[r];
        t.choice_count = 0;
        t.choices = nullptr;
        if (rows[r].choices) {
            const char16_t* const* labels = nullptr;
            t.choice_count = rows[r].choices(&labels);
            t.choices = reinterpret_cast<const uint16_t* const*>(labels);
        }
    }
    open_once = true;
    int count = 0;
    for (int r = 0; r < RowCount; ++r) {
        if (rows[r].screen != screen || (rows[r].shown && !rows[r].shown(v))) continue;
        screen_rows[count] = table[r];
        screen_row_index[count] = r;
        ++count;
    }
    *out = screen_rows.data();
    return count;
}

void Poll() {
    Apply(false);
}

/// Applies changed bytes: on/off and sliders always, choices (dropdowns write the hovered
/// entry) only when `choices`.
void Apply(bool choices) {
    std::scoped_lock lock{mutex};
    if (!open_once) return;
    bool changed = false;
    auto& v = BbSettings::Get();
    for (int r = 0; r < RowCount; ++r) {
        // What the game's widget wrote (the guest thread): a choice writes the int32, the
        // on/off and slider rows its low byte.
        const int32_t raw = reinterpret_cast<volatile int32_t&>(values[r]);
        const int32_t now = rows[r].kind == Choice ? raw : (raw & 0xff);
        // A choice applies when its dropdown has closed (cancel restores the value), or when
        // the screen closes; the dropdown writes the entry under the cursor while open.
        const bool settled = choices || rows[r].kind != Choice ||
                             (dropdown_open[r] && *dropdown_open[r] == 0);
        if (now != applied[r] && settled) {
            std::printf("Settings: game menu: %s %d -> %d\n", Ascii(rows[r].label).c_str(),
                        applied[r], now);
            applied[r] = now;
            rows[r].set(v, now);
            changed = true;
        }
    }
    if (changed) {
        BbSettings::Save();
    }
}

void WatchDropdown(const BbNativeSetting* row, const volatile uint8_t* open) {
    std::scoped_lock lock{mutex};
    const auto k = row - screen_rows.data();
    if (k >= 0 && k < RowCount) dropdown_open[size_t(screen_row_index[size_t(k)])] = open;
}

void ForgetDropdowns() {
    std::scoped_lock lock{mutex};
    dropdown_open.fill(nullptr); // the screen and its widgets are gone
}

} // namespace BbNative

extern "C" int bbgpu_native_settings(int32_t screen, const BbNativeSetting** rows) {
    return BbNative::Rows(screen, rows);
}

extern "C" const uint16_t* bbgpu_native_screen_text(int32_t screen, int32_t which) {
    static const char16_t* const texts[BB_NATIVE_SCREENS + 1][2] = {
        {u"Screen", u"Screen mode, resolution and frame rate."},
        {u"Advanced options", u"Motion blur, depth of field and other effects."},
        {u"Upscaling", u"Upscaler, render resolution and sharpening."},
        {u"Graphics", u"Screen, graphics quality and upscaling."},
    };
    return reinterpret_cast<const uint16_t*>(texts[std::clamp(int(screen), 0, int(BB_NATIVE_SCREENS))][which & 1]);
}

extern "C" void bbgpu_native_settings_commit(void) {
    BbNative::Apply(true);
    BbNative::ForgetDropdowns();
}

extern "C" void bbgpu_native_settings_dropdown(const BbNativeSetting* row,
                                               const volatile uint8_t* open) {
    BbNative::WatchDropdown(row, open);
}
