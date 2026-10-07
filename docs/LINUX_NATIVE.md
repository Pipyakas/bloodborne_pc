# Native Linux build and KDE launcher

On Bazzite-DX, the host compiler and a user-owned Homebrew/SDK prefix can build
and run bbport without Podman or Distrobox. No OS layering or reboot is needed.
Windows cross-builds still use their container.

```bash
bash tools/cross/setup-linux-native.sh
bash tools/cross/build-linux.sh --native master
```

The setup installs dependencies through Bazzite-DX's existing Homebrew and pinned
header-only dependencies into `~/.local/share/bbport-sdk`. Override that location
with `BB_NATIVE_SDK`. Homebrew defaults to `/home/linuxbrew/.linuxbrew`.
The native build uses the host GCC/G++, up to eight compile jobs, and the existing
isolated `.cross/linux-src` build checkout; it does not build uncommitted edits.
For a development build in your current checkout:

```bash
source tools/cross/linux-native-env.sh
bash build.sh
```

The existing master autobuild detects the SDK's `READY` marker and uses the native
backend automatically. `--container` explicitly selects the old fallback. Build
stamps include `dist/linux/BUILD_BACKEND`, so changing backend rebuilds even when
the commit is unchanged. Failed builds leave the last successful package intact.
`dist/linux/out/bb-probe` is the executable, not `dist/linux/bb-probe`.

## KDE application menu and Favorites

Create `~/.config/bbport/linux.env` (a trusted shell configuration):

```bash
BB_GAME_DIR='/absolute/path/to/your/extracted/CUSA03173'
# Optional; the default is ~/.local/share/bbport:
# BB_DATA_DIR='/absolute/path/to/persistent/linux-data'
```

Then run:

```bash
bash tools/cross/install-linux-desktop.sh
```

**Bloodborne (bbport)** appears in the application menu and KDE Favorites. The
stable `~/.local/bin/bbport-linux` entry launches the latest successful native
`dist/linux` package without rebuilding or using a container. Launches wait for
an in-progress build; a running game holds the package lock so autobuild retries
after it exits. Existing favorites are preserved; Plasma need not restart.

Settings, saves, mods and generated files stay in `~/.local/share/bbport`, outside
the replaced build directory. No Windows saves or research data are imported.
Logs are in `~/.local/share/bbport/out/desktop-launch.log`. D1 defaults to its AMD
RADV Vulkan driver when installed; an explicit `VK_DRIVER_FILES` overrides it.
