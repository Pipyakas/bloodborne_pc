#!/usr/bin/env bash
# Stable desktop target: always use the latest successful dist/linux build.
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
config=${XDG_CONFIG_HOME:-$HOME/.config}/bbport/linux.env
[[ ! -f $config ]] || source "$config"
export BB_DATA_DIR=${BB_DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/bbport}
mkdir -p "$BB_DATA_DIR/out" "$repo/.cross"
fail() {
    echo "$*" >&2
    command -v notify-send >/dev/null && notify-send 'Bloodborne (bbport)' "$*" || true
    exit 1
}
# Keep the packaged scripts/libraries intact for this entire launch.
exec 9>"$repo/.cross/linux-lock"
flock -s 9
dist=$repo/dist/linux
[[ -x $dist/out/bb-probe ]] || fail 'No Linux build yet. Run tools/cross/build-linux.sh --native master.'
[[ -f $dist/BUILD_BACKEND && $(cat "$dist/BUILD_BACKEND") == native ]] || fail 'The latest Linux build is a container build. Build with --native first.'
[[ -f ${BB_GAME_DIR:-}/eboot.bin ]] || fail 'Set BB_GAME_DIR in ~/.config/bbport/linux.env to your extracted game directory.'
export BB_GAME_DIR BB_PREBUILT=1 BB_PROBE=$dist/out/bb-probe
# D1 has a CUDA-only NVIDIA card as well as its display AMD card. Respect overrides.
if [[ -z ${VK_DRIVER_FILES:-} && -f /usr/share/vulkan/icd.d/radeon_icd.x86_64.json ]]; then
    export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.x86_64.json
fi
exec bash "$dist/run.sh" "$@" >> "$BB_DATA_DIR/out/desktop-launch.log" 2>&1
