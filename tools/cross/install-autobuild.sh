#!/usr/bin/env bash
# Installs a systemd user timer on the build host (d1) that builds every new `master` commit of
# this checkout for Windows (dist/windows, which the laptop pulls) and for Linux (dist/linux):
# tools/cross/build-windows.sh and build-linux.sh --if-changed master.
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
units=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user
mkdir -p "$units"
cat > "$units/bbport-wincross.service" <<UNIT
[Unit]
Description=bbport: Windows and Linux builds of master (tools/cross)

[Service]
Type=oneshot
Nice=10
WorkingDirectory=$repo
# '-': a failed Windows build must not skip the Linux one (the logs in .cross/ say what failed).
ExecStart=-$repo/tools/cross/build-windows.sh --if-changed master
ExecStart=$repo/tools/cross/build-linux.sh --if-changed master
UNIT
cat > "$units/bbport-wincross.timer" <<UNIT
[Unit]
Description=bbport: check master for new Windows/Linux builds every 2 minutes

[Timer]
OnBootSec=2min
OnUnitActiveSec=2min

[Install]
WantedBy=timers.target
UNIT
# Commits and merges on master in this checkout start the builds at once instead of at the next tick.
hooks=$(git -C "$repo" rev-parse --path-format=absolute --git-path hooks)
for hook in post-commit post-merge; do
    if [[ -e $hooks/$hook ]] && ! grep -q bbport-wincross "$hooks/$hook"; then
        echo "$hooks/$hook exists: not replaced (the timer still builds within 2 minutes)" >&2; continue
    fi
    cat > "$hooks/$hook" <<'HOOK'
#!/bin/sh
# tools/cross/install-autobuild.sh: build master for Windows and Linux now (bbport-wincross).
[ "$(git symbolic-ref --short -q HEAD)" = master ] && systemctl --user start --no-block bbport-wincross.service 2>/dev/null
exit 0
HOOK
    chmod +x "$hooks/$hook"
done
systemctl --user daemon-reload
systemctl --user enable --now bbport-wincross.timer
loginctl enable-linger "$USER" 2>/dev/null || true
systemctl --user list-timers bbport-wincross.timer --no-pager
