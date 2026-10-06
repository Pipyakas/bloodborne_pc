#!/usr/bin/env bash
# Builds the Windows port on Linux (d1) and stages a ready-to-run install in dist/windows:
# the tracked files of the commit, out/ with bb-probe.exe and every DLL it needs, and an
# embeddable Python for the launcher scripts. The laptop's Bloodborne.cmd pulls it with
# bbport-update.ps1. Usage: tools/cross/build-windows.sh [--if-changed] [commit (default: HEAD)]
# Needs podman. Builds in .cross/ (a detached worktree, so uncommitted edits are never shipped).
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
if_changed=; [[ ${1:-} == --if-changed ]] && { if_changed=1; shift; }
commit=$(git -C "$repo" rev-parse "${1:-HEAD}")
dist=$repo/dist/windows
mkdir -p "$repo/.cross" "$repo/dist"
exec 9>"$repo/.cross/lock"; flock -n 9 || { echo 'Another Windows build is running.' >&2; exit 0; }
if [[ -n $if_changed && -f $dist/BUILD ]] && grep -q "^$commit" "$dist/BUILD"; then exit 0; fi

# The toolchain image is rebuilt when its definition changes.
image=bbport-wincross:$(cat "$repo"/tools/cross/{Containerfile,msys2-pacman.conf,toolchain.cmake,x86_64-w64-mingw32-clang,x86_64-w64-mingw32-clang++} | sha256sum | cut -c1-12)
if ! podman image exists "$image"; then
    podman build -t "$image" -f "$repo/tools/cross/Containerfile" "$repo/tools/cross"
fi

src=$repo/.cross/src
if [[ ! -d $src/.git && ! -f $src/.git ]]; then
    git -C "$repo" worktree add --detach "$src" "$commit"
fi
git -C "$src" checkout -q --detach --force "$commit"
git -C "$src" submodule update -q --init --recursive
log=$repo/.cross/build.log
echo "Building $commit for Windows (log: $log)"
if ! podman run --rm -v "$src:/src:Z" -v "$repo/.git:$repo/.git:z" -w /src "$image" bash tools/cross/in-container.sh > "$log" 2>&1; then
    tail -40 "$log" >&2; echo "Windows build failed for $commit; dist/windows unchanged." >&2; exit 1
fi

# Stage a complete install, then swap it in.
python=$repo/.cross/python-embed.zip
if [[ ! -f $python ]]; then
    curl -fsSL -o "$python.part" https://www.python.org/ftp/python/3.14.0/python-3.14.0-embed-amd64.zip && mv "$python.part" "$python"
fi
stage=$repo/dist/.windows-staging; rm -rf "$stage"; mkdir -p "$stage/out" "$stage/python"
git -C "$src" archive "$commit" | tar -x -C "$stage"
cp "$src"/out/bb-probe.exe "$src"/out/bb-gpu-capabilities.exe "$stage/out/"
[[ -f $src/out/bbport-pkg.exe ]] && cp "$src/out/bbport-pkg.exe" "$stage/out/"
cp "$src"/out/runtime-dlls/*.dll "$stage/out/"
python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$python" "$stage/python"
cp "$repo/tools/cross/bbport-update.ps1" "$stage/"
echo "$commit $(date -u +%Y-%m-%dT%H:%M:%SZ) $(git -C "$src" log -1 --format=%s "$commit")" > "$stage/BUILD"
rm -rf "$dist.old"; [[ -d $dist ]] && mv "$dist" "$dist.old"; mv "$stage" "$dist"; rm -rf "$dist.old"
echo "dist/windows: $(cut -c1-12 "$dist/BUILD")"
