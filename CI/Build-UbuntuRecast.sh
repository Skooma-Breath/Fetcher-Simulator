#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 INSTALL_PREFIX" >&2
    exit 2
fi

install_prefix="$(realpath -m "$1")"
work_root="$(mktemp -d "${RUNNER_TEMP:-/tmp}/fetcher-recast.XXXXXX")"
source_root="$work_root/source"
build_root="$work_root/build"
trap 'rm -rf "$work_root"' EXIT

# Ubuntu 24.04 ships Recast 1.5.1 without the CMake package configuration that
# this OpenMW generation consumes. Build the upstream 1.6.0 release as pinned
# static libraries so the resulting .deb has no PPA-only Recast dependency and
# does not install or replace a system shared library.
recast_commit="6dc1667f580357e8a2154c28b7867bea7e8ad3a7"
git init --quiet "$source_root"
git -C "$source_root" remote add origin https://github.com/recastnavigation/recastnavigation.git
git -C "$source_root" fetch --quiet --depth 1 origin "$recast_commit"
git -C "$source_root" checkout --quiet --detach FETCH_HEAD
if [[ "$(git -C "$source_root" rev-parse HEAD)" != "$recast_commit" ]]; then
    echo "Recast source checkout did not match the pinned commit" >&2
    exit 1
fi

cmake -S "$source_root" -B "$build_root" \
    -D CMAKE_BUILD_TYPE=Release \
    -D CMAKE_INSTALL_PREFIX="$install_prefix" \
    -D CMAKE_POSITION_INDEPENDENT_CODE=ON \
    -D BUILD_SHARED_LIBS=OFF \
    -D RECASTNAVIGATION_DEMO=OFF \
    -D RECASTNAVIGATION_TESTS=OFF \
    -D RECASTNAVIGATION_EXAMPLES=OFF
cmake --build "$build_root" --parallel "$(nproc)"
cmake --install "$build_root"

test -f "$install_prefix/lib/cmake/recastnavigation/recastnavigation-config.cmake"
