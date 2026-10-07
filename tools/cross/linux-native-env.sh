# Source this before bash build.sh, or use build-linux.sh --native.
sdk=${BB_NATIVE_SDK:-${XDG_DATA_HOME:-$HOME/.local/share}/bbport-sdk}
brew_prefix=${BB_BREW_PREFIX:-/home/linuxbrew/.linuxbrew}
[[ -f $sdk/READY ]] || { echo 'Run tools/cross/setup-linux-native.sh first.' >&2; return 1; }
export PATH="$brew_prefix/bin:$PATH"
export CMAKE_PREFIX_PATH="$sdk:$brew_prefix${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export CPATH="$sdk/include${CPATH:+:$CPATH}"
# Zydis's package exports Zycore as a bare -l name rather than an absolute path.
export LIBRARY_PATH="$brew_prefix/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
export PKG_CONFIG_PATH="$brew_prefix/lib/pkgconfig:$brew_prefix/share/pkgconfig:$brew_prefix/opt/xorgproto/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
# Use the host compiler/standard library, not Homebrew LLVM's libc++.
export CC=/usr/bin/gcc CXX=/usr/bin/g++
export CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-8}
