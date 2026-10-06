# Pipyakas/bloodborne_pc — Windows-focused bbport fork

This fork builds on [deadinside28/bloodborne_pc](https://github.com/deadinside28/bloodborne_pc)
and the shared Windows-port work. It is not a rewrite of Bloodborne or a general
PS4 emulator: the original game executable runs through a game-specific runtime,
with a Vulkan renderer derived from shadPS4.

The [main README](README.md) contains the inherited technical background and
build instructions. This page defines **this fork's aims and release policy**, so
upstream's Linux results are not confused with this fork's Windows validation.

## Primary target: Windows executable releases after every master push

Every push to this fork's `master` should automatically build and test the Windows
port, then publish a downloadable GitHub Release containing the executable and
the redistributable files needed to run it. Users should not need a compiler or
MSYS2 development installation simply to try the latest completed work.

**Current status:** this is the release target, not an implemented guarantee.
There is currently no GitHub Actions release workflow in this repository. Source
is published to `master`, and the developer's local playable executable is rebuilt;
that does **not** create a downloadable binary release. Check
[Releases](https://github.com/Pipyakas/bloodborne_pc/releases) for actual published
downloads rather than assuming each source push already has an executable.

The intended automation must:

- Run for every push to `master`, including documentation-only pushes.
- Build from the pushed commit in a clean Windows environment and run appropriate
  automated tests before publishing. A failed build/test must be reported, not
  presented as a working release.
- Identify each release by its exact source commit and distinguish experimental
  development builds from any separately validated stable releases.
- Include `bb-probe.exe`, the launcher/preparation tools and required
  redistributable runtime dependencies, plus setup instructions and license
  notices. A bare EXE is not necessarily a runnable package.
- Keep games, decrypted game images, Sony modules, saves, personal settings,
  secrets and shader caches out of downloads. Proprietary upscaler components
  require separate license review; do not assume locally downloaded DLLs may be
  republished.

## What distinguishes this fork

- **Windows usability:** a native Windows build, setup/launch tools, native-menu
  integration and controller/input improvements.
- **Graphics and performance:** NVIDIA DLSS work alongside the inherited FSR/TAA
  paths, with correctness, frame pacing and laptop resource limits guiding changes.
  Feature availability depends on hardware and validation; this is not a claim
  that every upscaler or frame-generation mode works on every machine.
- **Non-disruptive agent testing:** MCP control, input and GPU screenshots without
  taking desktop focus. Agent launches default to minimized, silent windows so
  running instances remain visible in the taskbar.
- **Publish completed work:** tested changes are committed, integrated into
  `master`, built locally and pushed to this fork. Unfinished experiments are
  published on clearly labeled task branches, not passed off as ready master builds.

## Relationship to upstream

Upstream provides the native-execution runtime, game-specific renderer foundation,
Linux/Steam Deck tooling and substantial FSR/performance work. This fork focuses
on making its evolving Windows implementation easier to install, test and use.
Useful fixes from upstream and other forks should be reviewed and integrated
selectively; their benchmarks are not automatically Windows benchmarks.

Report problems with **this fork's builds** in
[this fork's issue tracker](https://github.com/Pipyakas/bloodborne_pc/issues),
including the release/commit, hardware, settings and a log with personal paths
removed. Do not send fork-specific support requests to shadPS4 or assume upstream
maintains this fork.

## Status and licensing

The port remains experimental. A successful automated build is not proof of a
complete play-through, long-term stability or compatibility with every PC. Release
notes must separate verified results from targets and known limitations.

You must supply your own legally obtained, decrypted Bloodborne v1.09 game data.
The existing [GPL license](LICENSE), upstream credits and third-party notices
remain applicable. This project is not affiliated with Sony, FromSoftware or the
upstream maintainers.
