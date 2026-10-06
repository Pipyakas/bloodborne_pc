# Less visible dynamic-resolution transitions

The previous controller fitted a target in integer percentages but could change
by 10–20 percentage points down or five up at once. Integer 1% precision was not
a one-point transition limit. Enabling DRS also started at native resolution,
and every resize restarted Halton camera jitter.

This change:

- Starts at the current render size (or configured preset on initial startup).
- Applies a maximum one-point transition in either direction, including rollback.
  A provider safety floor can still force a larger correction if necessary.
- Uses half-second measurement windows without the obsolete post-resize pause
  window. Ordinary overload must persist for two windows; severe overload can
  react on the first. Quality recovery remains slower, at least two seconds
  between upward adjustments, and keeps the existing oscillation protection.
- Discards menu/loading measurements rather than raising quality during them.
- Keeps the jitter sequence across DRS resizes. Explicit mode changes still
  restart it; TAA still resets render-sized history. DLSS range support determines
  whether its feature/history can be reused across sizes; this change does not
  manufacture unsupported driver capabilities.
- Treats missing/invalid GPU timings as no measurement, not infinite headroom.

## Verification

- `dynamic-resolution-test`: seed, bounds, one-point down/up/rollback,
  convergence, unsupported-provider floor corrections.
- `motion-history-test`: existing range/restart/matching/jitter tests passed.
- Full Windows worktree build succeeded.
- Minimized, silent runtime A/B through `tools/mcp/bbport_mcp.py`, a private copy
  of settings and saves, 3840x2160 output, DLSS E, fixed 50% before enabling DRS:
  - Baseline included `100 -> 90 -> 100 -> 80 -> 60 -> 40`; largest step 20.
  - Candidate seeded at 50 and moved in one-point steps down to 34, then back
    to 35 when lowering stopped helping; largest step 1.
  - Both reached gameplay. Candidate screenshots retained a correctly composed
    scene/HUD, and no assertion or Vulkan validation error was observed.
  - Original settings/saves hashed unchanged; test instances stopped afterward.

These tests verify transition magnitude and basic runtime correctness, not
imperceptibility in every scene or an FPS improvement. Smooth transitions respond
more slowly to a sudden large workload increase. Final subjective motion quality
still needs player evaluation, particularly with TAA or a driver whose DLSS
dynamic-range query is unavailable.
