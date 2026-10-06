#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include "gpu/shim/bbport_settings.h"
#include "gpu/shim/bbport_native_settings.h"
#include "gpu/shadps4/video_core/renderer_vulkan/scene_resolution.h"

int main(int argc, char** argv) {
    assert(argc == 2);
    // Never use an existing game/user configuration as the test fixture.
    assert(!std::filesystem::exists(argv[1]));
#ifdef _WIN32
    assert(_putenv_s("BB_CONFIG", argv[1]) == 0);
    assert(_putenv_s("BB_RENDER_RES", "") == 0);
#else
    assert(setenv("BB_CONFIG", argv[1], 1) == 0);
    assert(unsetenv("BB_RENDER_RES") == 0);
#endif
    assert(std::string(std::getenv("BB_CONFIG")) == argv[1]);
    auto& v = BbSettings::Get();
    v.upscaler = BbSettings::UpscalerDlss;
    v.preset = BbSettings::Quality;
    v.render_percent = 0;
    assert(std::abs(BbSettings::RenderPercent() - 100.0f / 1.5f) < 0.001f);
    const BbNativeSetting* rows;
    const int count = BbNative::Rows(BB_NATIVE_UPSCALING, &rows);
    const BbNativeSetting* slider = nullptr;
    for (int r = 0; r < count; ++r) {
        if (rows[r].kind == BB_NATIVE_SLIDER && rows[r].label[0] == 'R') slider = &rows[r];
    }
    assert(slider && !slider->choice_count);
    for (int step : {0, 1, 3, 5, 7, 10}) {
        *static_cast<int32_t*>(slider->value) = step;
        BbNative::Poll(); // no page-close/commit call
        const int percent = 50 + step * 5;
        assert(v.render_percent == percent);
        assert(BbSettings::RenderPercent() == float(percent));
        const auto size = Vulkan::SceneResolution::ForPercent(float(percent), {2560, 1440});
        assert(size.width % 2 == 0 && size.height % 2 == 0);
        assert(size.width == uint32_t(2560 * percent / 100));
        assert(size.height == uint32_t(1440 * percent / 100));
    }
    v.render_percent = 75;
    BbSettings::Save();
    v.render_percent = 0;
    BbSettings::Load();
    assert(v.render_percent == 75);
    v.upscaler = BbSettings::UpscalerTaa;
    assert(BbSettings::RenderPercent() == 100.0f);
    v.upscaler = BbSettings::UpscalerOff;
    assert(BbSettings::RenderPercent() == 100.0f);
    assert((Vulkan::SceneResolution::ForPercent(75, {1920,1080}) ==
            Vulkan::SceneResolution::Size{1440,810}));
    std::remove(argv[1]);
    std::puts("PASS: native render slider, immediate apply, persistence, dimensions, Off/TAA");
}
