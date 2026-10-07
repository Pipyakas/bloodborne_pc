#!/usr/bin/env bash
# Builds the native Linux port of a commit on the build host (d1) in a container, so every commit
# of master is known to build on Linux as well as Windows (tools/cross/build-windows.sh), and
# stages it in dist/linux: the commit's files plus out/bb-probe, bb-gpu-capabilities and
# out/gpu/libbbgpu.so. The binaries need the container's libraries (run them inside
# bbport-linuxbuild, or build natively). Usage: tools/cross/build-linux.sh [--if-changed] [commit]
# Needs podman. Builds in .cross/linux-src (a detached worktree: uncommitted edits never count).
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
if_changed=; [[ ${1:-} == --if-changed ]] && { if_changed=1; shift; }
commit=$(git -C "$repo" rev-parse "${1:-HEAD}")
dist=$repo/dist/linux
mkdir -p "$repo/.cross" "$repo/dist"
exec 9>"$repo/.cross/linux-lock"; flock -n 9 || { echo 'Another Linux build is running.' >&2; exit 0; }
if [[ -n $if_changed && -f $dist/BUILD ]] && grep -q "^$commit" "$dist/BUILD"; then exit 0; fi
# A commit that failed is not retried every timer tick; a new commit or a manual run retries.
if [[ -n $if_changed && -f $repo/.cross/linux-failed ]] && grep -q "^$commit" "$repo/.cross/linux-failed"; then exit 0; fi

image=bbport-linuxbuild:$(sha256sum < "$repo/tools/cross/Containerfile.linux" | cut -c1-12)
if ! podman image exists "$image"; then
    podman build -t "$image" -f "$repo/tools/cross/Containerfile.linux" "$repo/tools/cross"
fi

src=$repo/.cross/linux-src
if [[ ! -d $src/.git && ! -f $src/.git ]]; then
    git -C "$repo" worktree add --detach "$src" "$commit"
fi
git -C "$src" checkout -q --detach --force "$commit"
git -C "$src" submodule update -q --init --recursive
log=$repo/.cross/linux-build.log
echo "Building $commit for Linux (log: $log)"
# 8 CPUs: d1 is shared with other work.
if ! podman run --rm --cpus=8 -v "$src:$src:Z" -v "$repo/.git:$repo/.git:z" -w "$src" "$image" \
        bash -c 'git config --global --add safe.directory "*" && bash build.sh' > "$log" 2>&1; then
    tail -40 "$log" >&2; echo "$commit" > "$repo/.cross/linux-failed"
    echo "Linux build failed for $commit; dist/linux unchanged." >&2; exit 1
fi
rm -f "$repo/.cross/linux-failed"

stage=$repo/dist/.linux-staging; rm -rf "$stage"; mkdir -p "$stage/out/gpu"
git -C "$src" archive "$commit" | tar -x -C "$stage"
cp "$src"/out/bb-probe "$src"/out/bb-gpu-capabilities "$stage/out/"
cp "$src/out/gpu/libbbgpu.so" "$stage/out/gpu/"
echo "$commit $(date -u +%Y-%m-%dT%H:%M:%SZ) $(git -C "$src" log -1 --format=%s "$commit")" > "$stage/BUILD"
rm -rf "$dist.old"; [[ -d $dist ]] && mv "$dist" "$dist.old"; mv "$stage" "$dist"; rm -rf "$dist.old"
echo "dist/linux: $(cut -c1-12 "$dist/BUILD")"
