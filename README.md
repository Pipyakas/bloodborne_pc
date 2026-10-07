# Pipyakas/bloodborne_pc — Windows-focused fork

**Long-term goal: a portable, source-available Bloodborne implementation.**
Decomp work is part of this repository: [research](research/decomp/README.md),
[source layout](decomp/README.md), and [conversion roadmap](docs/DECOMP_ROADMAP.md).
The current playable build still needs the original x86-64 executable and the
shadPS4-derived renderer; a full source build and arbitrary-platform support do not exist yet.

**Start here: [Fork aims and release policy](README.fork.md).** Our primary target
is a downloadable Windows executable release after every successful `master` push.
Automatic GitHub binary releases are not implemented yet; a source push alone is
not a binary release. Report fork-specific issues in
[this repository](https://github.com/Pipyakas/bloodborne_pc/issues).

The technical background below is inherited from upstream and extended with this
fork's changes. Linux performance/validation claims do not establish Windows results.

**Upstream support note:** upstream requests that its questions go to
https://discord.gg/KYZRKk9CB, not to shadPS4's server. This fork is independently maintained.


# bbport — a native Linux port of Bloodborne

**English** · [Русский](README.ru.md)

bbport runs the original PlayStation 4 executable of *Bloodborne* (CUSA03173, game version
1.09) directly on an x86-64 Linux PC. It is not a general emulator. The game's own x86-64 code
executes natively; a small runtime written for this one game replaces the PS4 system libraries;
the GPU work is translated to Vulkan by a renderer derived from
[shadPS4](https://github.com/shadps4-emu/shadPS4) and heavily extended for this game, including
temporal upscaling with AMD FSR 3.1, FSR 4 and FSR 4.1.1.

> **No game files are included.** You need your own dump of Bloodborne (CUSA03173, v1.09).
> This project is not affiliated with Sony Interactive Entertainment, FromSoftware or AMD.

**Status: experimental, playable.** The game boots, loads saves and plays (the Hunter's Dream
and several areas of Yharnam were played with it) with sound, gamepad and saving.
A full play-through has not been verified, and only one machine (Linux, AMD Radeon RX 7800 XT,
Mesa/RADV) has been tested thoroughly.

## Highlights

- **Native execution.** The eboot is converted offline into a flat memory image; PS4 libc and
  libSceFios2 are linked into it as native code. No CPU emulation and no per-instruction
  translation: the game code runs at full speed.
- **Unlocked frame rate.** Community patches (`patches/Bloodborne.xml`) make the simulation
  use the real frame time; ~90 FPS at 4K with FSR 4 Balanced on an RX 7800 XT, ~150 FPS at
  1440p with FSR 4 Quality. Also 30/60/90 FPS modes.
- **Temporal upscaling built for this game.** Bloodborne has no velocity buffer, so bbport
  computes motion vectors itself: camera motion from depth and the scene matrices, and object
  motion (characters, cloth, weapons) from the vertex positions of the previous frame. The
  scene is jittered sub-pixel (Halton) and rendered at a reduced resolution; the upscaler fills
  the output (720p for the Steam Deck, 1080p, 1440p or 2160p) and the UI is drawn natively at the output resolution.
  - **FSR 3.1** (FireBurn/FSR-Vulkan).
  - **FSR 4 (INT8, model v07)** on GPUs exposing the required Vulkan shader features —
    RDNA2/3 included (see Requirements).
  - **FSR 4.1.1 (INT8)**: AMD's 4.1.1 DLL is recorded once under vkd3d-proton and its passes
    are replayed natively on Vulkan; the output is **bit-exact** with the DLL. The assets are
    built on your machine from your own DLLs (`tools/fsr4cap`).
  - Faster than AMD's own shaders on RDNA3: the final passes of FSR 4 and 4.1.1 were rewritten
    to store through workgroup memory (3.5× and 2.3× faster, bit-exact); FSR 4 costs ~4 ms at
    4K on an RX 7800 XT instead of ~6 ms.
- **Multi-threaded GPU command processing.** The PS4 command stream is decoded on one thread
  and draws are bound and recorded on another (two-stage pipeline), with a Vulkan recording
  thread and helper threads for memory copies. Early on the single GPU thread capped the game
  at ~26 FPS; now it runs at 90–150 FPS depending on resolution and scene.
- **In-game menu** (F1 or L3+R3): upscaler, preset, sharpness, output resolution, game
  effects (chromatic aberration, DoF, motion blur, SSAO, the game's own AA, SSR, model LOD).
- **GTK4 launcher** and an **AppImage** for the Steam Deck.

## How it differs from shadPS4

| | shadPS4 | bbport |
|---|---|---|
| Scope | General PS4 emulator, many games | One game: Bloodborne v1.09 |
| Loading | Its own ELF loader and kernel emulation at run time | The eboot is converted offline (`scripts/`) into an image with PS4 libc/Fios2 linked in; a C loader maps it and jumps into the game (loader and runtime: ~5k lines) |
| System libraries | Broad HLE of the PS4 OS | A small runtime (`src/runtime_*.c`) that implements exactly what Bloodborne calls: memory, threads, sync, files, audio (incl. ATRAC9), pad, saves, AppContent |
| GPU | shadPS4 video core and shader recompiler | The same core (vendored, GPL) with ~200 marked changes (`bbport:`) plus new modules: two-stage draw pipeline, render-state and texture-set memoization, render-scale proxies, motion vectors, FSR 3.1/4/4.1.1, frame capture and GPU profiler |
| GPU thread | One thread processes the whole command stream (the bottleneck in Bloodborne) | Decode and draw recording run on separate threads; the work scales with the hardware threads (Steam Deck included) |
| Upscaling | — | Temporal (FSR 3.1, FSR 4, FSR 4.1.1) with the game's own motion vectors and jitter |
| Game patches | Patch files applied by the emulator | The same community patches, compiled at start (`scripts/patches.py`); render resolution, effects and FPS from the launcher |

Without shadPS4 there would be no bbport: its renderer and shader recompiler are the base of
the graphics side.

## Requirements

- Linux x86-64, a Vulkan 1.3 GPU. Tested: AMD RX 7800 XT with Mesa 26 (RADV).
  FSR 4 / 4.1.1 require shader Float16, Int8/Int16, integer dot products, linear compute
  derivatives and extended storage image formats; FSR 4.1.1 additionally requires
  `VK_VALVE_shader_mixed_float_dot_product`. Unsupported choices fall back to FSR 3.1
  before the first frame and are disabled in the in-game menu.
- Your decrypted game dump: the `CUSA03173` folder (eboot.bin, sce_module, ...), version 1.09.
- To build: GCC, CMake, Ninja, Python 3, glslang, SDL3, Vulkan headers and the libraries in
  `shell.nix`. With [Nix](https://nixos.org) everything comes from `shell.nix` automatically.

## Build and run

```bash
git clone --recursive <this repository> bbport && cd bbport
bash build.sh                        # builds out/bb-probe and out/gpu/libbbgpu.so
BB_GAME_DIR=/path/to/CUSA03173 bash run.sh
```

or the launcher (pick the game folder, settings, *Start*):

```bash
bash launcher/bb-launcher.sh         # launcher/install-desktop.sh adds it to the app menu
```

By default the game folder is expected next to the repository (`../CUSA03173`). Saves and the
shader cache go to `user/` (the launcher lets you choose another folder); settings to
`bbport.ini`. A gamepad is used through SDL3; there is a keyboard fallback.

**Resolution and preset changes:** for outputs other than 1080p (720p on the Steam Deck,
1440p, 4K) the whole game renders at the preset's resolution, set by a patch at start — the
fastest path. Changing the output or the preset in the in-game menu then needs *Apply and
restart the game*. The *Live resolution changes* setting (launcher, in-game menu,
`bbport.ini` `live_resolution=0|1|auto`; **off by default**) instead keeps the game at
1080p internally and scales its render targets at run time, so 720p/1080p/1440p/4K and the
presets switch without a restart. It costs more: the game then believes it renders 1080p
and draws more (e.g. ~8× more small lights), and some targets are copied between sizes —
use it on strong desktop GPUs only (`auto` turns it on for discrete GPUs with 8+ GB that are
not pre-Turing NVIDIA). 1080p output and TAA always use the live path.

**TAA:** a separate native-resolution temporal AA mode in the launcher and overlay,
switchable live without an FSR model. The saved FSR preset is restored when returning
to FSR. FSR Native AA adds reconstruction on top of full-resolution rendering and can
be slower than disabling AA. TAA also adds work compared with no temporal AA.
The RCAS switch and the 0–2 sharpness control also work with TAA. Sharpening runs after
temporal accumulation and leaves its history and HUD unchanged.

**Mods:** the launcher accepts separate loose-file mod folders (with `dvdroot_ps4/`, an extra
wrapper folder, or the game folders such as `chr/` directly; file name case does not matter),
with enable switches and load order. A sibling `CUSA03173-mods/` overlay also works.
The original game is preserved; later mods override conflicting files.
**Third-party patches:** shadPS4-format XML patch files in the data directory's `patches/`,
switched on and off in the launcher. See [mods and patches](docs/MODS.md).

**Launcher language:** Russian or English (follows the system language by default).

**Free camera and game debug menu** (v1.09): enable the corresponding switches in the
launcher or in-game menu and restart. Free camera uses Lance McDonald's
[GoldHEN patch](https://github.com/GoldHEN/GoldHEN_Patch_Repository/blob/main/patches/xml/Bloodborne-Orbis.xml):
hold Cross and press L3 to cycle modes (keyboard: hold Space and press Z). It needs no fonts
and conflicts with *Enemy Control*.
For the game debug menu, install `DbgFont14h.ccm` and `DbgFont14h.tpf` from
[Debug Menu and XML Patch](https://www.nexusmods.com/bloodborne/mods/253) into the game's
`dvdroot_ps4/font/` first. Startup rejects missing or empty font files instead of launching
the unsafe patch. Open it with the left touchpad / G; Backspace is the right touchpad.
Touch coordinates are forwarded from SDL gamepads; Back/Select emulates a left click on
pads without a touch surface. The port's settings menu is F1 / L3+R3.

GPU occlusion queries still use synthetic pixel counters (`PixelPipeStatDump`), and
`IT_SET_PREDICATION` is unimplemented. Free camera allows visual investigation; it does
not implement GPU occlusion culling.

**Upscaler assets** (not included; FSR 3.1 needs none):

```bash
bash tools/fetch_fsr4_assets.sh      # FSR 4 v07 (MIT, built from AMD's source by Q2RTX)
# FSR 4.1.1, from your own AMD DLLs (e.g. OptiScaler's FSR4_LATEST), needs Proton (GE-Proton):
bash tools/fsr4cap/build_assets.sh <amd_fidelityfx_upscaler_dx12.dll> <amd_fidelityfx_loader_dx12.dll>
```

**AppImage** (Steam Deck): `bash build.sh && bash packaging/appimage.sh` →
`dist/Bloodborne-bbport-x86_64.AppImage`; data in `~/.local/share/bbport`, `--play` starts the
game without the launcher window (Game Mode). FSR 4.1.1 models are not packaged: build them
(see above) into `~/.local/share/bbport/fsr4_411` (`BB_PACKAGE_FSR411=1` bundles a local
`fsr4_411` into an AppImage for your own devices). On the Steam Deck pick the 1280×720 output (the
game is 16:9; on the 1280×800 screen it gets thin bars).

**Adding the AppImage to Steam** (*Add a Non-Steam Game*) needs no options; the compatibility tool
does not matter. (Steam preloads its overlay into every non-Steam game; the AppImage removes it
before its own programs start, so the Steam overlay is not shown in the game.) Where Steam
runs games without FUSE (NixOS: Steam's FHS sandbox; the AppImage then exits with *Cannot mount
AppImage*), set the launch options to

```
TMPDIR=$HOME/.cache APPIMAGE_EXTRACT_AND_RUN=1 NO_CLEANUP=1 %command%
```

The AppImage then unpacks itself (~2 GB, `~/.cache/appimage_extracted_*`) on the first start
(~10 s) and reuses that copy afterwards; a new AppImage version gets a new copy, the old one can
be deleted. Without `TMPDIR` it would unpack into Steam's `/tmp`, which is in RAM there. Add
` --play` after `%command%` to skip the launcher.

**NVIDIA in the AppImage:** startup discovers the host's installed 64-bit NVIDIA Vulkan ICD
and exposes its vendor libraries alongside the bundled AMD/Intel drivers. This keeps the
NVIDIA userspace driver matched to the host kernel module. Standard Linux distributions keep
these libraries under `/usr/lib*`; on NixOS the AppImage's internal `/nix/store` may hide them.
In that case copy the NVIDIA libraries into an accessible directory and set
`BB_NVIDIA_LIB_DIR=/path/to/libraries` (the NVIDIA manifest must also be accessible).
Explicit `VK_DRIVER_FILES`/`VK_ICD_FILENAMES` overrides are preserved. Diagnose drivers inside
the package with:

```bash
./Bloodborne-bbport-x86_64.AppImage --vulkan-info 2>&1 | tee bbport-vulkan.log
```

A user reported successful startup with FSR 3 on a GTX 1060 6GB (Fedora 44, NVIDIA
580.178.04); selecting FSR 4 caused a black window. Use FSR 3 on this configuration.

MangoHud is bundled in the AppImage; enable its checkbox in the launcher.
When running from source, install MangoHud separately. A diagnostic launch with
`VK_LOADER_LAYERS_DISABLE=~implicit~` also disables MangoHud.

Useful variables: `BB_FRAME_STATS=1` (frame statistics), `BB_GPU_PROFILE=1` (GPU time per
pass), `BB_FSR4_PROFILE=1` (GPU time per FSR 4 pass), `BB_UPSCALER=taa|fsr3|fsr4|fsr411|off|none`,
`BB_FRAMES_AHEAD=N` (how many frames the GPU command thread may run ahead of the GPU; 1 by default,
0 = unbounded), `BB_PRESENT_THREAD=0` (present on the vblank thread, as before),
`BB_LIVE_RES=1` (live resolution changes instead of the startup patch for outputs other than 1080p),
`BB_PAD_RECORD=file` / `BB_PAD_REPLAY=file` (record a route with F9, replay it in scripted tests),
`BB_GC_BUDGET_MB=N` (texture cache budget, as on integrated GPUs), `BB_PRESENT_DUMP_TRIGGER=file`
with `BB_PRESENT_DUMP_COUNT=N` (dump N consecutive presented frames), `BB_HIDDEN=1` (the window
is never shown and the host keyboard and gamepads are ignored), `BB_MINIMIZED=1` (create a minimized
taskbar window without taking focus, ignoring fullscreen settings and host input; silent unless
`BB_AUDIO` is set; `BB_HIDDEN=1` takes precedence), `BB_CONTROL=port` (a line protocol
on 127.0.0.1 for pad input, frame waits, PNG screenshots and text entry; 0 picks a free port;
commands in `src/runtime_control.c`), `BB_IME_TEXT=...` (answer the next system text dialog
immediately with this name instead of asking).

**Agents (MCP):** `tools/mcp/bbport_mcp.py` (registered in `.mcp.json`, Python standard library
only) starts the game in the background with `BB_MINIMIZED=1`, `BB_AUDIO=none`, no console and a
60 FPS limit, and gives agents `game_launch`, `game_screenshot`, `game_press`, `game_hold`,
`game_release`, `game_wait`, `game_log`, `game_text`, `game_commands`, `game_status` and
`game_stop`. Screenshots are read back from the presenter, so the desktop and its focus stay
untouched; the log is `out/mcp/game.log`. The same tools run from a shell, one call each, the
game running in between: `python tools/mcp/bbport_mcp.py launch`, `... press tokens=cross`,
`... screenshot` (prints the PNG's path), `... commands='["wait 120","ime Hunter"]'`, `... stop`,
`... help`.
Launches default to a minimized taskbar window, so running instances stay visible in the taskbar.
You can restore it manually; agent input still uses the control channel and host input stays disabled.
Use `game_launch(hidden=true)` or `python tools/mcp/bbport_mcp.py launch hidden=true` to hide it
completely. Use `minimized=false` (with `hidden=false`) for a normal foreground window. Minimized
launches do not provide foreground focus for features that require it (such as DLSS Frame Generation).
More in [docs/](docs); recent changes: [docs/CHANGES_2026-10-02.md](docs/CHANGES_2026-10-02.md),
[docs/CHANGES_2026-10-03.md](docs/CHANGES_2026-10-03.md).

## Windows (experimental)

The same port also builds natively for 64-bit Windows 10 (1803 or newer) and 11 with
[MSYS2](https://www.msys2.org)'s CLANG64 toolchain (clang, libc++, lld). Tested: RTX 4090
(NVIDIA 616.86), i9-13900K, Windows 11 24H2; it boots, creates and loads saves and plays with
sound (input was tested through `BB_PAD_FILE`; gamepads and the keyboard go through SDL as on
Linux), 110–120 FPS at 1080p in Iosefka's Clinic (~3,200 draws per frame, GPU command thread
2.5 µs per draw). v1.09 dumps of other regions work as well (tested: CUSA00900).

**Built on Linux, played on Windows:** a Linux machine can build the Windows port and a Windows
PC only runs it. `tools/cross/build-windows.sh [commit]` (needs podman) cross-compiles with host
clang against MSYS2's CLANG64 packages (the same libraries a native build uses) in a container,
then stages a complete install in `dist/windows`: the commit's files, `out\bb-probe.exe` with
every DLL it needs, and an embeddable Python for the launcher scripts, so the PC needs neither
MSYS2 nor a compiler. `tools/cross/install-autobuild.sh` adds a systemd user timer that rebuilds
`dist/windows` whenever `master` moves. On the PC, an install folder holds `Bloodborne.cmd`
(the setup program's launcher with one extra line that runs `bbport-update.ps1` first): it
asks the build machine over SSH (`ssh d1` by default; `BBPORT_REMOTE`, `BBPORT_REMOTE_DIST`)
for its latest build and copies it in when it changed, keeping `bbport.ini`, saves, mods,
the game folder and the DLSS files; when the build machine is off, the installed build starts.

**Setup program:** `setup.bat` opens a window to choose the game folder and the settings
(output resolution, frame rate, upscaler and preset, effects, game language, optional DLSS and
FSR 4 model downloads, shortcuts). Install / Update then installs MSYS2 to `C:\msys64` when it
is missing (another folder: set `BB_MSYS2`), the packages below and the submodules, builds the
port, and writes `bbport.ini`, `Bloodborne.cmd` (the launcher: frame rate, game language and
present mode, then `run.bat`) and Desktop and Start menu shortcuts with the game's icon. Run it
again to change the settings; Save settings writes them without building. It needs nothing but
Windows: `setup.bat` compiles `tools\setup\BbportSetup.cs` with the C# compiler of .NET Framework
4 into `out\bbport-setup.exe`. The steps below do the same by hand.

**First launch:** when no game folder is known yet (none given, none remembered in
`out\game_dir.txt`), `run.bat` first opens the first-launch screen of `bb-probe.exe`
(`--first-run`): choose the game folder, or the game's .pkg files and where to install them
(next to the executable by default, or another folder); the game then starts from that folder.
Agent runs (`BB_HIDDEN=1` / `BB_MINIMIZED=1`) and `BB_FIRST_RUN=0` skip it.

**From .pkg files:** without a dump, *Install from .pkg files...* in the setup window installs
the game from its package and the 1.09 update package (fake-signed packages, as shadPS4
installs) into `<chosen folder>\CUSA…`, which becomes the game folder: the game first, then the
update's files over it, ~32 GB, about a minute on an SSD. `out\bbport-pkg.exe` (built by `build.sh` and `setup.bat`) does the same
from a command line (`bbport-pkg install <folder> <game.pkg> <update.pkg>`, `bbport-pkg info
<pkg>`). The installer (`tools\setup\PkgInstall.cs`) is a C# port of the orbis-pkg, orbis-pfs
and orbis-pkg-util crates (MIT/Apache-2.0) that shadps4-game-manager uses; delta updates and
add-ons are not supported.

1. Install MSYS2 to `C:\msys64` (another folder: set `BB_MSYS2`) and, in an MSYS2 shell:

   ```
   pacman -S --needed git mingw-w64-clang-x86_64-{clang,lld,libc++,cmake,ninja,pkgconf,python,sdl3,boost,fmt,glslang,spirv-cross,spirv-headers,vulkan-headers,vulkan-loader,vulkan-memory-allocator,xxhash,zydis,robin-map,ffmpeg}
   ```

2. `git clone --recursive <this repository> bbport`, then from `cmd` or Explorer:

   ```
   run.bat --game-dir D:\Games\CUSA03173
   ```

   The first start builds the port (a few minutes; `build.sh` in the CLANG64 environment) into
   `out\bb-probe.exe`. The game folder is remembered: afterwards `run.bat` alone starts the game.
   `BB_PREBUILT=1` skips the build check. Settings, saves, mods and patches use the same files as
   on Linux (`bbport.ini`, `user\`, `mods\`, `patches\`); the in-game menu (F1 or L3+R3)
   changes the settings. `fullscreen=1` in `bbport.ini` (or F11 in the game) gives a
   borderless window at the desktop size; with `output_res=3840x2160` and `preset=1` (FSR 3.1
   Quality, scene 2560x1440) the RTX 4090 above stays at the 120 Hz display limit. The GTK
   launcher and the AppImage are Linux-only.

**DLSS on NVIDIA RTX:** `upscaler=dlss` needs NVIDIA's model next to the executable
(`bash tools/fetch_dlss.sh`, or the setup program's DLSS download). `dlss_model` (menu: *DLSS
model*) picks the network: `auto` (the driver's choice per preset: K, M for Performance, L for
Ultra Performance), `e` (DLSS 3 CNN), `j`/`k` (DLSS 4 Transformer), `m`/`l` (DLSS 4.5
Transformer 2). The menu shows the preset in use. Cost at 2560x1440 output from 1280x720 on an
RTX 3070 Laptop: CNN E ~1.4 ms, Transformer K ~2.7 ms, Transformer 2 M ~5.5 ms per frame; on
RTX 20/30 the transformer models are noticeably heavier than on RTX 40/50.

**Render resolution and dynamic resolution:** with the upscaler off or TAA (no presets),
`render_scale=5..200` (menu: *Render resolution*, steps of 5) renders the scene at that percent of
the output and scales it to the output under full-resolution HUD; above 100 it supersamples.
`dynamic_resolution=1` (menu: *Dynamic*, the last entry of *Upscaling quality* or *Render
resolution*) replaces the preset: it lowers the render
resolution (at most one percentage point per half-second measurement; the manual `render_scale`
uses 5%) while the GPU misses the frame rate limit (or is within 5% of it), until the
CPU limits the frame rate or a step no longer saves GPU time (under a tenth of what its pixel count
predicts: the remaining work is the upscaler and HUD at output size, shadows), and raises it again
while the GPU has headroom, up to 100% of the output (a `render_scale` above 100: that).
It measures the GPU's busy time per frame (a timestamp at the start and end of each command
buffer) against the frame interval: when the CPU limits the frame rate the GPU idles part of each
frame and the resolution is not lowered. DLSS keeps one feature per quality mode across the
sizes its range accepts (50-100% of the output for Quality, Balanced and Performance).
DRS starts from the current fixed render size rather than jumping to native. Isolated timing
spikes are ignored (sustained severe overload can react immediately); quality recovery is slower
than lowering. Menu/loading frames do not raise or lower the scene resolution. Ordinary DRS
changes preserve the jitter sequence and retain upscaler history where the provider supports
dynamic render sizes; TAA still resets its render-sized history when resized. Smoother transitions
trade some convergence speed for less visible popping.

**Frame generation (DLSS-G, Windows):** `frame_gen=2x|3x|4x|dynamic` (menu: *Frame generation*;
switching it on or off needs a restart, the multiplier changes live) presents through a D3D12/DXGI
swapchain with NVIDIA Streamline instead of the Vulkan swapchain: the frame is drawn into Vulkan
images shared with D3D12, and the upscaler's depth, motion vectors and HUD-less scene are tagged
for DLSS-G (`vk_frame_gen.cpp`). `dynamic` is NVIDIA's dynamic multi-frame generation toward the
display refresh rate; the fixed modes cap the rendered frame rate at refresh / multiplier with
Reflex. It needs an upscaler (DLSS, FSR or TAA), hardware-accelerated GPU scheduling, and:

- `bash tools/fetch_streamline.sh`: Streamline 2.14.1's signed DLLs into `out\streamline\`;
- on RTX 20/30, the [dlssg_sm86](https://github.com/sdli1995/dlssg_for_sm86) mod: its `version.dll` (310.9 build) and
  `dlssg_sm86.ini` in `out\dlssg_sm86\` (`BB_DLSSG_MOD` names another file). bbport loads it
  before Streamline, as a game would through its proxy DLL. RTX 40/50 need no mod.

DLSS-G pauses while the window is not focused (its own rule) and in menus and loading screens.
Outputs other than 1080p also give it the scene without the HUD; at 1080p the UI is part of what
it interpolates. Anything missing leaves the Vulkan swapchain in use; the menu says why.
Verified on an RTX 3070 Laptop with dlssg_sm86 0.3.5: 2x, 3x, 4x and dynamic present 2, 3, 4 and
up to 4 frames per rendered frame. `BB_SL_LOG=1` prints Streamline's log.

**Laptops:** CPU and GPU share one power budget, and tools like G-Helper or Armoury Crate lock
the GPU clock per profile (`nvidia-smi -lgc`): on battery, G-Helper's *Silent* profile capped an
RTX 3070 Laptop at 800 MHz (35 FPS where AC gave 50-60). Use the charger and a performance
profile; at 1440p the GPU (not the CPU) is the limit on that machine.

**What this fork adds on Windows** (over upstream's port):

- *System text dialog on screen.* Every PS4 system text box (the name you enter when a new game
  starts, for instance) is now a centered input box over a dimmed frame, drawn by the overlay
  (`gpu/shim/bbport_overlay.cpp`), instead of text typed into the window title bar. Enter or OK
  confirms, Escape or Cancel gives the original name back; the game's own dialog blocks your pad
  input meanwhile. The old title-bar text entry and its state machine in `gpu/shim/window.cpp`
  are gone. `BB_IME_TEXT=Vincent` answers the next dialog without asking (useful for scripted
  runs).
- *Agent launches show the overlay.* A minimized window (`BB_MINIMIZED=1`) draws the settings
  menu and the text dialog into the captured frame, so a screenshot taken through
  `tools/mcp/bbport_mcp.py` shows what a player would see. Previously the overlay was drawn only
  into the swapchain, which a minimized window never presents.
- *`ime` on the control channel.* `BB_CONTROL` gained an `ime <name>` command that opens the same
  dialog the game opens (`sceImeDialogInit`), so the box can be exercised without playing to a
  naming screen. `status` reports `text_input: 1` while it is open.

Everything else in the Windows section above is upstream's; see `README.fork.md` for the fork's
aims and the release policy.

How it differs from Linux, all on the Win32 API directly (no POSIX layer): the guest address
space is reserved at start as one placeholder and mapped with section views
(`src/win32_memory.c`); the runtime's locks, condition variables, threads and clocks are SRW
locks, Windows condition variables, CRT threads and QueryPerformanceCounter (`src/host_sync.h`,
`src/runtime_host.c`), and libc++ maps the GPU library's `std::mutex`/`std::thread` to the same;
GPU page tracking and crash reports run in a vectored exception handler; the guest's thread
pointer lives in a TEB TLS slot (`patch_tls_reads` in `src/probe.c`); `src/win32_compat.c`
covers file system and time zone details. With libc++ on Windows, `std::thread::get_id()` and
`std::jthread::joinable()` ask the kernel (`GetThreadId`): hot paths keep their own flags.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | Loader (`probe.c`) and the HLE runtime |
| `scripts/` | Offline preparation of the game image, module linking, patch compiler |
| `gpu/` | Renderer library: vendored shadPS4 video core with this port's changes (`gpu/VENDOR.txt`), shims, ImGui menu, FSR 4.1.1 runtime (`gpu/shadps4/video_core/renderer_vulkan/fsr411`) |
| `launcher/`, `packaging/` | GTK4 launcher; Nix package and AppImage |
| `patches/` | Community patches for Bloodborne |
| `tools/` | Developer tools: scripted runs, A/B toggles, FSR benchmark helpers, FSR 4 shader rewrites, `fsr4cap` (FSR 4.1.1 recording/extraction) |
| `tests/` | Loader, runtime, patch and renderer tests |
| `docs/` | Design notes and measurements ([upscaler](docs/upscaler.md), [parallel GPU](docs/parallel_gpu.md), [motion vectors](docs/motion_vectors.md), [roadmap](docs/ROADMAP.md)) |

Tests: `bash build.sh --test`, `python3 -m unittest discover -s tests`, and
`ninja -C out/gpu motion-history-test ui-composition-test scene-resolution-test motion-shader-test`.

## Roadmap

- More CPU parallelism in GPU command processing (split the draw-recording stage further),
  scaling to all hardware threads — most important for the Steam Deck.
- Async compute for the upscaler (the frame is GPU-bound at 4K).
- XeSS (super resolution) and XeFG frame generation through a Wine helper sharing Vulkan
  memory (a memory-bridge prototype is in `tools/bridge_helper`); DLSS for NVIDIA users;
  inputs exposed so that OptiScaler-style mapping works.
- Frame generation (FSR 3.1 FG first), reactive and transparency masks for particles and fog.
- Fix the races in AMD's FSR 4.1.1 shaders at output widths that are not multiples of 64
  (e.g. 1600×900), as already done for the left-edge race in FSR 4 v07 at 1080p.
- Steam Deck validation of the AppImage; HDR output.

## Credits and licenses

bbport is licensed under the **GNU GPL v2 or later** ([LICENSE](LICENSE)) — it contains code
from shadPS4 (GPL-2.0-or-later). Third-party components keep their licenses:
[shadPS4](https://github.com/shadps4-emu/shadPS4) video core and shader recompiler (GPL-2.0+),
[sirit](https://github.com/shadps4-emu/sirit), [half](https://half.sourceforge.net/),
[FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) by FireBurn (MIT; FSR 3.1 on Vulkan and the
FSR 4 v07 provider), AMD FidelityFX SDK (MIT), [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9)
(MIT), [Dear ImGui](https://github.com/ocornut/imgui) (MIT), DejaVu fonts,
[dxil-spirv](https://github.com/HansKristian-Work/dxil-spirv) (MIT, used to build the
FSR 4.1.1 assets). Game patches by Kyo, Lance McDonald, auser1337, illusion, emoose and other
community members (`patches/Bloodborne.xml`). AMD's FSR 4 DLLs and model data are not
distributed here.
