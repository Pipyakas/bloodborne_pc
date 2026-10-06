# Native menu changes awaiting runtime validation

Task branches have been consolidated into `master`; this file records unfinished
requests, not an alternate playable build. Use the normal Start-menu shortcut.

## Render-resolution integration

The merge from master retains its newer `render_scale`/`dynamic_resolution`
implementation, including smooth dynamic transitions and upscaler history.
The unverified `render_percent` prototype from `bf1f56d` is superseded; its code
and standalone test are retained in that historical commit, not the current tree.
Using both percentage settings would create competing render-size policies.

The requested native custom-percentage slider still needs adapting to master's
menu/settings model. This merge does not claim that UI request is finished.

## Offline main-menu Back

The `cf2f216` candidate restored a flag the title builder disables, but testing
the integrated master build showed that Circle still stayed on the offline main
menu. The ineffective hook has been removed; the historical commit is preserved.
A working Back transition remains unfinished. It would not implement online services.

## Consolidation verification

CLANG64 master build passed. A minimized, silent run reached the native title/main
menus at approximately 60 FPS. Circle was tested on the offline main menu; no
fault occurred, but navigation did not change. The test instance was stopped.
Testing used isolated configuration and user directories; hashes of the real
settings/save files were unchanged. Uncommitted decomp-harness backup work and
FSR profiling edits are preserved outside this consolidation.
