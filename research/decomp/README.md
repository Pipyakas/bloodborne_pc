# Bloodborne functional decompilation — experimental research

This directory is part of the main repository's source-conversion effort. It is
**not a standalone decompiled game**, and none of these notes makes a replacement
function ready for integration. The older `decomp` branch is a historical snapshot,
not the required home for future work.

The goal is a portable, source-available game implementation, reached through
incremental, behavior-tested function replacement inside bbport and eventual
removal of the original executable and shadPS4 translation dependency.
See [the roadmap](../../docs/DECOMP_ROADMAP.md),
[Ghidra workflow](../../docs/DECOMP_GHIDRA.md), and the reproducible
[inventory tool](../../tools/decomp/inventory.py).
Use the [conversion target map](CONVERSION_TARGETS.md) to connect these findings
to current bbport code, proposed source boundaries and acceptance tests. Future
reviewed implementation belongs under [`decomp/`](../../decomp/README.md).

## Published snapshot

The topic folders contain analyst-written findings, hypotheses, addresses and
limited explanatory instruction excerpts for CUSA03173 v1.09. They are a
2026-10-06 snapshot of research, not independently certified conclusions.
Confidence labels and unresolved objections in each note still apply.

- `allocator/`: allocator behavior and candidate layouts.
- `classes/`: provisional vtable/class relationships. A table is not necessarily
  a distinct class; a constructor-like store does not prove inheritance.
- `frame_timing/`: frame-loop/timestep and patch-site analysis. In particular,
  the proposed explanation for a ~120 FPS ceiling remains unproven.
- `fps_compare/`: the high-FPS sprint slowdown (root cause and fix), FromSoftware's DRS
  parameters in later games, TAA notes. Reference-game files stay private.
- `renderer/`: front-end target candidates and hooks, not a recovered renderer.
- `resolution_menus/`: resolution/menu targets and possible patch replacements.
- `strings/`: string-reference naming methodology. Hints are not original symbols.

The notes refer to scripts, CSVs and intermediate artifacts in the private
research workspace. Those references document provenance; they do **not** mean
those artifacts are included here. Historical references to private-only work
describe the policy when the analysis was produced; this curated publication
was subsequently authorized by the owner.

## Private inputs and unfinished work

No game executable, ELF, linked image, assets, saves, captured memory pages,
Ghidra project, full pseudo-C/disassembly export or proprietary SDK is included.
Users must supply their own lawfully obtained game inputs to run the tooling.
Reverse engineering can qualify for legal exceptions in some circumstances;
that is not a blanket assurance that redistributing the game's code is fair use.

The unfinished harness, class-map v2 corrections and middleware work are not
included in this snapshot. The older class findings must be read as provisional
until the correction pass is reviewed. Failed/rejected model output is not
published as a verified deliverable.

## Placement and worker policy

All decomp workers, harness development, GPU/runtime testing, Ghidra processing
and private analysis/backup live on d1. Preserved Windows decomp data is a backup,
not an active worker checkout.

GPT-6.1, including all aliases and variants, is excluded from the subagent and
detached-worker pool. A stuck free worker does not authorize a paid fallback.
This does not restrict a model explicitly selected by the user as the main
composer. Keep experimental results off playable `master` until validated.
