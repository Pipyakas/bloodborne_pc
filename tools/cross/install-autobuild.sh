#!/usr/bin/env bash
# Installs a systemd user timer on the build host (d1) that rebuilds dist/windows whenever
# `master` of this checkout moves (tools/cross/build-windows.sh --if-changed master).
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
units=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user
mkdir -p "$units"
cat > "$units/bbport-wincross.service" <<UNIT
[Unit]
Description=bbport: Windows build of master for the Windows PC (tools/cross)

[Service]
Type=oneshot
Nice=10
WorkingDirectory=$repo
ExecStart=$repo/tools/cross/build-windows.sh --if-changed master
UNIT
cat > "$units/bbport-wincross.timer" <<UNIT
[Unit]
Description=bbport: check master for a new Windows build every 2 minutes

[Timer]
OnBootSec=2min
OnUnitActiveSec=2min

[Install]
WantedBy=timers.target
UNIT
systemctl --user daemon-reload
systemctl --user enable --now bbport-wincross.timer
loginctl enable-linger "$USER" 2>/dev/null || true
systemctl --user list-timers bbport-wincross.timer --no-pager
