# Bloodborne source-conversion roadmap

## Goal and current state

Convert bbport from native execution of the PS4 game with a shadPS4-derived GPU
translator into a **source-available Bloodborne implementation with portable
platform backends**. Decompilation is part of this repository, not an isolated
research branch. Windows usability remains a near-term product goal.

"Build for any platform" is the architectural objective: compile the same game
logic for a new CPU/OS by implementing its backend, rather than running the PS4
x86-64 executable. It is not a promise of support for every device. Graphics,
toolchain, platform restrictions and hardware capabilities still need validation.

Today bbport still loads original game code and Sony modules, and translates GNM
through the shadPS4-derived renderer. **No complete reconstructed game source,
source-only game build or non-x86 game build has been demonstrated.**

## Repository layout

- [`research/decomp/`](../research/decomp/README.md): curated evidence, hypotheses
  and [research-to-source targets](../research/decomp/CONVERSION_TARGETS.md).
- [`decomp/`](../decomp/README.md): intended reviewed source, interfaces, temporary
  bridges and tests; currently documentation only, no admitted replacements.
- [`tools/decomp/`](../tools/decomp/): existing inventory and Ghidra tooling.
- `src/` and `gpu/`: current playable runtime/renderer, retained as the integration
  and comparison environment while replacement coverage grows.

Reviewed research and tooling can live on `master` without activating game code.
Preserve existing unfinished task work; do not create new task branches or enable
unverified replacements. Verified implementations can join `master` following
the shared-checkout/playable-build workflow in `AGENTS.md`. The old
`decomp` branch is a publication snapshot, not an architectural boundary.

## Strategy: incremental replacement, then independence

1. Establish native capture/replay on d1 and a reliable original-game reference.
2. Reconstruct bounded functions/subsystems, with temporary x86-64 System V ABI
   bridges into the original game. Functional equivalence, not byte matching.
3. Keep recovered logic separate from guest addresses, ABI structs and host APIs;
   make data ownership and platform service contracts explicit.
4. Verify replacements and integrate reversible switches while the original game
   remains available for comparison. Remove a patch/hook only when its behavior
   and consumers are covered.
5. Replace executable middleware and system-module dependencies, and recover the
   initialization/data contracts needed to stop loading original game code.
6. Replace guest GNM generation/translation with a native rendering interface.
   This is **required for the final goal**, not an optional postscript.
7. Prove a standalone source build, then validate additional OS/CPU backends.

The original game dump remains the user's local asset source. Source availability
does not imply redistribution of artwork, audio, maps or other game content.

## Milestones and acceptance gates

### M0 — trustworthy verification infrastructure (in progress)

Existing tools: function inventory/call graph and Ghidra preparation/export.
The measured v1.09 FDE table has 162,959 entries; these are not a count of verified
game functions or reconstruction progress. Class and other notes are provisional.

Unfinished: Linux-native replacement registration, capture/replay and real-game
leaf proof. Windows `CONTEXT` vectors are not native Linux vectors. d1 Vulkan
enumeration works with RADV on its RX 6700 XT; use
`VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.x86_64.json`. The NVIDIA card is
intentionally CUDA-only. Enumeration is not proof that game capture works.

Acceptance: original/replacement/independent expectations agree on fresh vectors;
an injected incorrect implementation fails; unsupported ABI, platform, memory or
execution cases are rejected, not silently skipped. Captures stay private.

### M1 — evidence-backed types and portable leaves

Recover signatures, layouts, ownership and side effects; start with bounded math,
containers, strings and allocator helpers. Generated pseudo-C and candidate
vtables are inputs for review, never automatically compiled source.

Acceptance: differential and edge-case tests, explicit dependencies, no guest ABI
in portable logic, and shareable synthetic tests that run without the game dump.
Record verified functions separately from drafted or statically researched ones.

### M2 — source-owned game/engine subsystems

Priority targets: timing, renderer front-end data, resolution/scene setup, menus,
allocator, resource loading, then threading, input, parameters/saves, animation,
EzState/AI and gameplay. Use the target map rather than unrelated address lists.

Acceptance: dependency-complete subsystem tests and repeatable gameplay routes;
list exactly which binary patches/hooks are retired. Timing and performance
claims need measurements; source reconstruction alone promises no FPS gain.

### M3 — middleware and platform contracts

Inventory Havok, Scaleform, FMOD, Lua, zlib, libpng and system modules. Review
licenses and compatible public-source implementations or replacements; establish
asset-format, physics, animation, UI and audio behavior tests.

Binary middleware may serve as a temporary bridge but **cannot satisfy** the full
source build or arbitrary-CPU goal. Build host files, threading, input, audio and
graphics behind explicit interfaces; preserve save compatibility deliberately.

### M4 — standalone source game, no original executable

Acceptance: build/run without original eboot, linked executable images, Sony
modules or proprietary executable middleware. Initialize globals, tables and
registries from reviewed source or documented asset extraction. Demonstrate that
there is no hidden code-loading fallback; retain an optional comparison build.

### M5 — native renderer, no shadPS4 translation dependency

Render from explicit scene/material/animation data through a native backend,
rather than rebuilding a GNM stream and translating it. Recover shader/material
semantics and review shader-source/extraction provenance as part of this work.

Acceptance: scene/HUD and motion correctness, resize/resource lifetime, multiple
graphics settings and gameplay coverage; standalone build excludes the vendored
GNM translator. Preserve attribution/licenses for any reused source.

### M6 — demonstrated portability

Build/test on supported Windows and Linux configurations first; add another CPU
architecture and other OS/graphics backends only when implemented and tested.
Audit pointer width, alignment, endian assumptions, atomics, SIMD, serialization,
calling conventions and exception behavior. Platform capability gaps must have
explicit fallbacks or be reported unsupported.

Acceptance: publish exact tested OS/CPU/GPU/backend combinations and build
instructions. A cross-compile alone is not a working game port.

## Publication, placement and review

- Public: curated research, reviewed tooling and reconstructed implementations
  with provenance/license review and honest validation status.
- Private on d1: game binaries/assets/saves, memory captures, Ghidra projects,
  bulk pseudo-C/disassembly and proprietary SDKs. GPL on the repository does not
  establish redistribution rights for third-party game or middleware code.
- All decomp workers and harness/runtime testing run on d1. Preserve Windows
  snapshots; do not restart Windows decomp workers.
- Eligible free `ccgw/*` research workers only, never `ccgw-k12/*` or GPT-6.1
  workers/aliases. Read the live catalog and canonical d1 scorecard; cap fan-out
  around three and independently verify results. This does not restrict the
  user's selected main composer.
- Record evidence and uncertainty, not model confidence. Review alone cannot
  substitute for equivalence tests; synthetic tests alone cannot establish
  real-game equivalence. Native implementations also need platform validation.

Track verified/replaced coverage, remaining binary dependencies, negative-test
results, gameplay regressions and portable build coverage. No completion date or
function-throughput estimate is established. A full source conversion is a large
engineering effort; the first useful milestone is a verified replacement that
actually removes an existing patch or runtime dependency.
