# Reconstructed game source

This is the intended home for reviewed Bloodborne source reconstruction, not a
dump of Ghidra output. **No reconstructed game implementation is admitted here
yet.** The current executable still runs original game code.

See the [conversion roadmap](../docs/DECOMP_ROADMAP.md) and
[research-to-source target map](../research/decomp/CONVERSION_TARGETS.md).

## Intended layout (create with actual implementations, not placeholders)

| Path | Responsibility |
|---|---|
| `include/` | Evidence-backed types and platform-neutral interfaces |
| `game/` | Timing, gameplay, menus, AI and game-state logic |
| `engine/` | Resource loading, scene state, animation and memory policies |
| `platform/` | Host files, threads, input, audio, window and graphics backends |
| `bridge/` | Temporary original-game ABI adapters and replacement registration |
| `tests/` | Public synthetic tests and locally supplied private-vector tests |

Portable logic must not depend on image-relative addresses, Windows `CONTEXT`,
PS4 system calls, x86-64 instructions or a Vulkan-only host API. Those concerns
belong in temporary bridges or explicit backends. Binary-layout structs used by
the bridge are not automatically the native source model: convert pointers,
ownership and serialization deliberately.

Each admitted implementation needs provenance, dependencies, unresolved
assumptions, differential-test evidence, negative tests and a link to the old
patch/hook/path it replaces. Compile verified replacements through an explicit
build target; directory presence alone must never activate a replacement.

No game binaries/assets/saves, captured memory, proprietary SDKs or bulk generated
decompiler output belong here. Source publication needs provenance/license review;
the repository's GPL does not grant rights to third-party game or middleware code.
