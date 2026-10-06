#!/usr/bin/env bash
# Downloads NVIDIA Streamline (the SDK release from github.com/NVIDIA-RTX/Streamline, MIT; the
# DLSS-G model nvngx_dlssg.dll in it is under NVIDIA's RTX SDK license) for frame generation
# (frame_gen in bbport.ini, Windows only): the signed production DLLs go to out/streamline/.
# On RTX 20/30 frame generation also needs the dlssg_sm86 mod (out/dlssg_sm86/version.dll and
# dlssg_sm86.ini; see README "Frame generation").
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
version=v2.14.1
dest=${1:-out}/streamline
mkdir -p "$dest"
if [[ -s $dest/sl.interposer.dll && -s $dest/sl.dlss_g.dll ]]; then echo "Streamline: $dest present"; exit 0; fi
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
curl -fsSL --retry 3 -o "$tmp/sl.zip" \
    "https://github.com/NVIDIA-RTX/Streamline/releases/download/$version/streamline-sdk-$version.zip"
for name in sl.interposer.dll sl.common.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll nvngx_dlssg.dll; do
    unzip -p "$tmp/sl.zip" "bin/x64/$name" > "$dest/$name"
    [[ -s $dest/$name ]] || { echo "Streamline: bin/x64/$name missing in the release" >&2; exit 1; }
done
unzip -p "$tmp/sl.zip" license.txt > "$dest/LICENSE-Streamline.txt" 2>/dev/null || true
echo "Streamline $version: $dest"
