// SPDX-License-Identifier: GPL-2.0-or-later
// Saved FSR 4 choices must be safe before the first rendered frame on older GPUs.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <unistd.h>
#include "bbport_settings.h"

int main() {
    using namespace BbSettings;
    char path[] = "/tmp/bbport-upscaler-test-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    setenv("BB_CONFIG", path, 1);
    unsetenv("BB_UPSCALER");
    unsetenv("BB_FSR4_MODEL");
    unsetenv("BB_UPSCALE_PRESET");
    unsetenv("BB_RENDER_RES");
    auto& s = Get();
    s.upscaler = UpscalerTaa;
    s.preset = Performance;
    assert(RenderPreset() == NativeAA && s.preset == Performance);
    Save();
    s.upscaler = UpscalerOff;
    Load();
    assert(s.upscaler == UpscalerTaa && s.preset == Performance);
    assert(RenderPreset() == NativeAA && !ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr3;
    assert(RenderPreset() == Performance);
    for (bool fsr4 : {false, true}) {
        for (bool dlss : {false, true}) {
            for (int requested = 0; requested < UpscalerCount; ++requested) {
                FILE* config = std::fopen(path, "w");
                assert(config);
                std::fprintf(config, "upscaler=%s\npreset=3\noutput_res=2560x1440\n",
                             UpscalerName(requested));
                std::fclose(config);
                s.fsr4_problem = nullptr;
                Load();
                assert(s.upscaler == requested);
                ConfigureUpscalerSupport(fsr4, true, dlss);
                const bool unsupported = (requested == UpscalerFsr4 && !fsr4) ||
                                         (requested == UpscalerDlss && !dlss);
                assert(s.upscaler == (unsupported ? UpscalerFsr3 : requested));
                assert(s.fsr4_supported == fsr4 && s.fsr4_dll_supported == fsr4);
                assert(bool(s.fsr4_problem.load()) == unsupported);
                assert(s.preset == Performance && s.output_res == 2);
                // Startup patch settings still describe the already applied guest patches.
                assert(s.startup_upscaler == requested);
                ConfigureUpscalerSupport(fsr4, true, dlss);
                assert(s.upscaler == (unsupported ? UpscalerFsr3 : requested));
            }
        }
    }
    // The model is a setting of FSR 4: saved, and the DLL model's missing features are no
    // reason to leave FSR 4 (v07 runs instead).
    for (int model = 0; model < Fsr4ModelCount; ++model) {
        s.upscaler = UpscalerFsr4;
        s.fsr4_model = model;
        Save();
        s.fsr4_model = -1;
        Load();
        assert(s.upscaler == UpscalerFsr4 && s.fsr4_model == model);
        s.fsr4_problem = nullptr;
        ConfigureUpscalerSupport(true, false, false);
        assert(s.upscaler == UpscalerFsr4 && !s.fsr4_dll_supported && !s.fsr4_problem.load());
    }
    assert(CompareVersions("4.1.1.3529", "4.0.2") > 0 && CompareVersions("4.0.2", "4.0.2.0") == 0);
    assert(CompareVersions("4.1.1.2740", "4.1.1.3529") < 0 && CompareVersions("4.10", "4.9") > 0);
    // Highest: the DLL model when installed, supported and newer than the bundled one.
    s.fsr4_model = Fsr4ModelHighest;
    s.fsr4_sdk_version = "4.0.2";
    s.fsr4_dll_version = nullptr;
    ConfigureUpscalerSupport(true, true, false);
    assert(Fsr4PreferredModel() == Fsr4ModelSdk);
    s.fsr4_dll_version = "4.1.1.3529";
    assert(Fsr4PreferredModel() == Fsr4ModelDll);
    s.fsr4_dll_version = ""; // a set from before manifests
    assert(Fsr4PreferredModel() == Fsr4ModelDll);
    s.fsr4_dll_version = "4.0.1";
    assert(Fsr4PreferredModel() == Fsr4ModelSdk);
    s.fsr4_dll_version = "4.1.1.3529";
    ConfigureUpscalerSupport(true, false, false);
    assert(Fsr4PreferredModel() == Fsr4ModelSdk);
    s.fsr4_model = Fsr4ModelDll;
    assert(Fsr4PreferredModel() == Fsr4ModelDll); // chosen: tried, the renderer explains
    {
        FILE* config = std::fopen(path, "w");
        assert(config);
        std::fputs("upscaler=fsr4\nfsr4_model=auto\n", config);
        std::fclose(config);
        Load();
        assert(s.fsr4_model == Fsr4ModelHighest);
    }
    // Settings from before fsr4_model: upscaler=fsr411 was the DLL model.
    {
        FILE* config = std::fopen(path, "w");
        assert(config);
        std::fputs("upscaler=fsr411\npreset=3\n", config);
        std::fclose(config);
        s.fsr4_model = Fsr4ModelHighest;
        Load();
        assert(s.upscaler == UpscalerFsr4 && s.fsr4_model == Fsr4ModelDll);
        setenv("BB_UPSCALER", "fsr411", 1);
        s.fsr4_model = Fsr4ModelSdk;
        Load();
        assert(s.upscaler == UpscalerFsr4 && s.fsr4_model == Fsr4ModelDll);
        setenv("BB_FSR4_MODEL", "sdk", 1);
        Load();
        assert(s.fsr4_model == Fsr4ModelSdk);
        unsetenv("BB_UPSCALER");
        unsetenv("BB_FSR4_MODEL");
    }
    s.startup_preset = Quality;
    s.startup_upscaler = UpscalerFsr3;
    s.upscaler = UpscalerFsr3;
    s.preset = Performance;
    assert(RenderPreset() == Performance && !ResolutionNeedsRestart());
    setenv("BB_RENDER_RES", "1706x960", 1);
    assert(RenderPreset() == Quality && ResolutionNeedsRestart());
    s.preset = Quality;
    assert(!ResolutionNeedsRestart());
    s.upscaler = UpscalerOff;
    assert(ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr4;
    assert(!ResolutionNeedsRestart()); // provider change at the same patched size is live
    s.upscaler = UpscalerTaa;          // TAA needs native guest targets
    assert(ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr4;
    const int output = s.startup_output_res;
    s.output_res = (output + 1) % OutputCount;
    assert(ResolutionNeedsRestart());
    s.output_res = output;
    setenv("BB_RENDER_RES", "", 1);
    assert(!FixedRenderSession() && !ResolutionNeedsRestart());
    unsetenv("BB_RENDER_RES");
    std::remove(path);
    std::puts("Upscaler support: saved choices and FSR 3.1 fallback PASS");
}
