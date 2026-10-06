# Native menu changes awaiting runtime validation

These changes are on `native-menu`, not the playable master build.

## Custom render percentage

The native Upscaling screen replaces the quality dropdown with a render-resolution
slider: 50–100% of output width and height, in 5% steps. Its native 0–10 scale is
explained by the help text (0 = 50%, 10 = 100%). Output resolution is unchanged.
The setting is `render_percent`; 0 retains the legacy `preset` setting. Existing
settings are not converted until the slider is changed. The ImGui fallback offers
the custom percentage too; selecting a legacy quality preset there clears the
custom override.

FSR/DLSS use the custom dimensions, rounded to even pixels for half-resolution
effects. Slider changes apply without closing the page. Off and TAA stay at 100%.
An explicit startup-patched `BB_RENDER_RES` session still requires a restart;
the patch preparation code understands custom percentages as well.

Tests: standalone `tests/test_render_percent.cpp` covers immediate native slider
application, persistence, dimensions and Off/TAA behavior. Run it with a disposable
configuration-file path as its argument. `python -m unittest discover -s tests
-p test_patches.py` covers startup patch dimensions. CLANG64 worktree build passes.
In-game layout, live GPU transitions, DLSS/FSR output and cancellation still need
runtime validation. In particular, a legacy Ultra Performance preset below 50%
remains active until the user moves the new slider.

## Offline main-menu Back

The candidate restores the native main-menu list's Cancel flag, which the original
title builder disables. It only changes that list, not nested settings dialogs or
the initial Online/Offline list. Syntax checking passes, but the resulting native
transition must be verified in a game run before this can be called a working
Circle/Esc Back action. It does not implement online services.

Runtime testing is deferred while the shared GPU benchmark lock is active.
