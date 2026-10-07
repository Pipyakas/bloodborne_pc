#!/usr/bin/env bash
# Bazzite-DX: user-owned dependencies; no root, OS layering, or container.
set -euo pipefail
brew=${BB_BREW:-/home/linuxbrew/.linuxbrew/bin/brew}
command -v "$brew" >/dev/null || { echo 'Homebrew is required (Bazzite-DX includes it).' >&2; exit 1; }
brew_prefix=$("$brew" --prefix)
export PATH="$brew_prefix/bin:$PATH"
export HOMEBREW_NO_AUTO_UPDATE=1
"$brew" install cmake ninja pkgconf boost fmt sdl3 zydis miniz robin-map ffmpeg xxhash glslang libx11 xorgproto
sdk=${BB_NATIVE_SDK:-${XDG_DATA_HOME:-$HOME/.local/share}/bbport-sdk}
mkdir -p "$sdk/src"
for spec in KhronosGroup/Vulkan-Headers:v1.4.365 Neargye/magic_enum:v0.9.7 GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator:v3.3.0 herumi/xbyak:v7.24.2; do
    project=${spec%%:*}; tag=${spec##*:}; source=$sdk/src/${project##*/}-$tag
    [[ -d $source ]] || git clone --depth 1 --branch "$tag" "https://github.com/$project.git" "$source"
    "$brew_prefix/bin/cmake" -S "$source" -B "$source/build" -G Ninja \
        -DCMAKE_INSTALL_PREFIX="$sdk" -DMAGIC_ENUM_OPT_BUILD_EXAMPLES=OFF -DMAGIC_ENUM_OPT_BUILD_TESTS=OFF
    "$brew_prefix/bin/cmake" --install "$source/build"
done
touch "$sdk/READY"
echo "Native SDK ready: $sdk"
