#!/usr/bin/env bash
# Builds the Linux port of a commit on the build host (d1), so every commit
# of master is known to build on Linux as well as Windows (tools/cross/build-windows.sh), and
# stages it in dist/linux: the commit's files plus out/bb-probe, bb-gpu-capabilities and
# out/gpu/libbbgpu.so. Uses the native user SDK when installed, otherwise a container.
# Usage: tools/cross/build-linux.sh [--if-changed] [--native|--container] [commit]
# Builds in the existing .cross/linux-src (uncommitted edits never count).
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
if_changed=; backend=auto
while [[ ${1:-} == --* ]]; do
    case $1 in
        --if-changed) if_changed=1 ;;
        --native) backend=native ;;
        --container) backend=container ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done
sdk=${BB_NATIVE_SDK:-${XDG_DATA_HOME:-$HOME/.local/share}/bbport-sdk}
if [[ $backend == auto ]]; then
    if [[ -f $sdk/READY ]]; then backend=native; else backend=container; fi
fi
commit=$(git -C "$repo" rev-parse "${1:-HEAD}")
dist=$repo/dist/linux
mkdir -p "$repo/.cross" "$repo/dist"
exec 9>"$repo/.cross/linux-lock"; flock -n 9 || { echo 'Linux build or playable launch holds the package lock; retry later.' >&2; exit 0; }
if [[ -n $if_changed && -f $dist/BUILD && -f $dist/BUILD_BACKEND ]] &&
    grep -q "^$commit" "$dist/BUILD" && [[ $(cat "$dist/BUILD_BACKEND") == "$backend" ]]; then exit 0; fi
# A commit that failed is not retried every timer tick; a new commit or a manual run retries.
if [[ -n $if_changed && -f $repo/.cross/linux-failed ]] && grep -qx "$commit $backend" "$repo/.cross/linux-failed"; then exit 0; fi

image=bbport-linuxbuild:$(sha256sum < "$repo/tools/cross/Containerfile.linux" | cut -c1-12)
if [[ $backend == container ]] && ! podman image exists "$image"; then
    podman build -t "$image" -f "$repo/tools/cross/Containerfile.linux" "$repo/tools/cross"
fi

src=$repo/.cross/linux-src
if [[ ! -d $src/.git && ! -f $src/.git ]]; then
    git -C "$repo" worktree add --detach "$src" "$commit"
fi
git -C "$src" checkout -q --detach --force "$commit"
git -C "$src" submodule update -q --init --recursive
log=$repo/.cross/linux-build.log
echo "Building $commit for Linux ($backend; log: $log)"
# Never reuse CMake's compiler/dependency cache across host/container toolchains.
previous=$(cat "$repo/.cross/linux-backend" 2>/dev/null || echo container)
if [[ $previous != "$backend" && -d $src/out ]]; then
    mv "$src/out" "$src/out-$previous-$(date +%s)"
fi
echo "$backend" > "$repo/.cross/linux-backend"
build_linux() {
    if [[ $backend == native ]]; then
        source "$repo/tools/cross/linux-native-env.sh" || return 1
        # build.sh invokes ninja directly: limit it without changing shared source.
        wrappers=$repo/.cross/native-bin
        mkdir -p "$wrappers"
        printf '#!/bin/sh\nexec "%s/bin/ninja" -j8 "$@"\n' "$brew_prefix" > "$wrappers/ninja"
        chmod +x "$wrappers/ninja"
        export PATH="$wrappers:$PATH"
        (cd "$src" && bash build.sh)
    else
        podman run --rm --cpus=8 -v "$src:$src:Z" -v "$repo/.git:$repo/.git:z" -w "$src" "$image" \
            bash -c 'git config --global --add safe.directory "*" && bash build.sh'
    fi
}
# 8 CPUs: d1 is shared with other work.
if ! (set -e; build_linux) > "$log" 2>&1; then
    tail -40 "$log" >&2; echo "$commit $backend" > "$repo/.cross/linux-failed"
    echo "Linux build failed for $commit; dist/linux unchanged." >&2; exit 1
fi
rm -f "$repo/.cross/linux-failed"

stage=$repo/dist/.linux-staging; rm -rf "$stage"; mkdir -p "$stage/out/gpu"
git -C "$src" archive "$commit" | tar -x -C "$stage"
cp "$src"/out/bb-probe "$src"/out/bb-gpu-capabilities "$stage/out/"
cp "$src/out/gpu/libbbgpu.so" "$stage/out/gpu/"
echo "$commit $(date -u +%Y-%m-%dT%H:%M:%SZ) $(git -C "$src" log -1 --format=%s "$commit")" > "$stage/BUILD"
echo "$backend" > "$stage/BUILD_BACKEND"
rm -rf "$dist.old"; [[ -d $dist ]] && mv "$dist" "$dist.old"; mv "$stage" "$dist"; rm -rf "$dist.old"
echo "dist/linux: $(cut -c1-12 "$dist/BUILD")"
