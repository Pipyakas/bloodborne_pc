#!/usr/bin/env bash
# Runs inside the bbport-wincross container (tools/cross/build-windows.sh): build.sh with the
# cross toolchain, then copies the DLLs bb-probe.exe needs from the MSYS2 sysroot to out/runtime-dlls.
set -euo pipefail
export BB_TARGET=windows CC=x86_64-w64-mingw32-clang CXX=x86_64-w64-mingw32-clang++
export PKG_CONFIG_LIBDIR=/opt/msys/clang64/lib/pkgconfig:/opt/msys/clang64/share/pkgconfig PKG_CONFIG_SYSROOT_DIR=/opt/msys
export CMAKE_TOOLCHAIN_FILE=/opt/cross/toolchain.cmake
git config --global --add safe.directory "*"
bash build.sh
python3 tools/cross/dll_closure.py /opt/msys/clang64/bin out/runtime-dlls out/bb-probe.exe out/bb-gpu-capabilities.exe
