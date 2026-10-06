# Native menu changes awaiting runtime validation

These changes are on `native-menu`, not the playable master build.

## Render-resolution integration

The merge from master retains its newer `render_scale`/`dynamic_resolution`
implementation, including smooth dynamic transitions and upscaler history.
The unverified `render_percent` prototype from `bf1f56d` is superseded; its code
and standalone test are retained in that historical commit, not the current tree.
Using both percentage settings would create competing render-size policies.

The requested native custom-percentage slider still needs adapting to master's
menu/settings model. This merge does not claim that UI request is finished.

## Offline main-menu Back

The candidate restores the native main-menu list's Cancel flag, which the original
title builder disables. It only changes that list, not nested settings dialogs or
the initial Online/Offline list. Syntax checking passes, but the resulting native
transition must be verified in a game run before this can be called a working
Circle/Esc Back action. It does not implement online services.

Do not merge this Back candidate into playable master without runtime validation.
