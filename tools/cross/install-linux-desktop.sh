#!/usr/bin/env bash
# Register the native playable build and add it to KDE Kickoff Favorites.
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
bin=$HOME/.local/bin
apps=${XDG_DATA_HOME:-$HOME/.local/share}/applications
mkdir -p "$bin" "$apps"
printf '#!/usr/bin/env bash\nexec bash %q "$@"\n' "$repo/tools/cross/launch-linux.sh" > "$bin/bbport-linux"
chmod +x "$bin/bbport-linux"
cat > "$apps/bbport-linux.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Bloodborne (bbport)
Comment=Play the latest successful native Linux master build
Exec="$bin/bbport-linux"
Icon=applications-games
Terminal=false
Categories=Game;ActionGame;
StartupNotify=false
DESKTOP
command -v desktop-file-validate >/dev/null && desktop-file-validate "$apps/bbport-linux.desktop"
command -v kbuildsycoca6 >/dev/null && kbuildsycoca6 --noincremental
# Use KDE's supported API: preserve all existing favorites, no Plasma restart.
busctl --user call org.kde.ActivityManager /ActivityManager/Resources/Linking \
    org.kde.ActivityManager.ResourcesLinking LinkResourceToActivity sss \
    org.kde.plasma.favorites.applications applications:bbport-linux.desktop :global
echo 'Installed Bloodborne (bbport) in the application menu and KDE Favorites.'
