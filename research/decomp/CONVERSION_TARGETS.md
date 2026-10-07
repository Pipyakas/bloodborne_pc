# Research → source conversion targets

These are implementation targets, **not verified replacements**. Addresses and
layout claims in the linked notes retain their confidence labels. Each future
implementation should link back here and record the dependency it actually retires.

| Research | Current dependency to replace | Source boundary / required proof |
|---|---|---|
| [Frame timing](frame_timing/notes.md) | `scripts/patches.py`, timestep sites in `patches/Bloodborne.xml` | Explicit simulation clock and timestep consumers; replay movement/animation at multiple rates, test pause and frame spikes; do not assume the alleged 120 FPS cause is proven |
| [High FPS, DRS, TAA comparison](fps_compare/notes.md) | Sprint stuck-check patch (`tools/patch_asm/sprint_slowdown.s`), DRS controller in `dynamic_resolution.h` | Per-second movement rates in source; movement replay at 30/60/120 FPS matching the 30 FPS trace; DRS with history kept across resizes |
| [Renderer](renderer/notes.md), [hooks](renderer/hook_points.md) | Camera/pass inference in `gpu/shadps4/video_core/renderer_vulkan/`, guest GNM command generation | Explicit scene, camera, transforms and render-pass data; image/motion-vector comparison before removing a heuristic; then native backend without GNM translation |
| [Resolution and menus](resolution_menus/notes.md), [patch mapping](resolution_menus/patch_replacements.md) | Resolution XML/immediate patches, `src/runtime_menu.c`, `src/runtime_effects.c`, scene-size proxies | Owned render/UI extents, settings and menu state; verify resize lifetime, aspect, culling, text receipt, Continue/Load/New/Cancel paths before deleting hooks |
| [Allocator](allocator/notes.md) | Guest allocator and memory assumptions serviced by `src/runtime_memory.c` | Explicit allocation/ownership policy; allocation/free/reuse, alignment, failure, concurrent use and map transitions; do not copy provisional struct guesses as established layouts |
| [Classes](classes/notes.md) | Address-based object/vtable identification | Evidence-backed types and lifecycle; distinguish embedded objects from inheritance; reviewed correction pass and runtime layout tests required |
| [Strings](strings/notes.md) | Anonymous function/module labels | Navigation and task selection; names are hints, not verified semantics or permission to emit headers |

Middleware is an additional prerequisite: inventory Havok, Scaleform, FMOD and
other dependencies; decide compatible public-source replacement or reviewed
reimplementation, then test the game's observable behavior and data compatibility.
No reviewed middleware completion report is included in this snapshot.

## What a completed target must report

1. Original build/hash and address convention; evidence and uncertainty.
2. Dependencies, inputs/outputs, ownership, side effects and threading model.
3. Portable implementation path and any temporary host/guest bridge.
4. Original-versus-replacement comparison plus independent expectations and an
   injected-bug test proving the harness rejects incorrect output.
5. Gameplay checkpoints/image checks where relevant; preserved saves/settings.
6. Patches, hooks, executable code or middleware actually removed, and remaining
   binary dependencies. An address catalogue alone is not conversion progress.

Private raw evidence stays on d1. Publish small reviewed summaries and shareable
synthetic tests; never commit captures merely to make an evidence link work.
