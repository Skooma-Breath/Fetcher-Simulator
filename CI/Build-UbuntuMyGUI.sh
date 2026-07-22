#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 INSTALL_PREFIX" >&2
    exit 2
fi

install_prefix="$(realpath -m "$1")"
work_root="$(mktemp -d "${RUNNER_TEMP:-/tmp}/fetcher-mygui.XXXXXX")"
source_root="$work_root/source"
build_root="$work_root/build"
trap 'rm -rf "$work_root"' EXIT

# Ubuntu 24.04 ships MyGUI 3.4.2, but this OpenMW generation uses the
# std::string_view virtual interface from MyGUI 3.4.3. Build only the upstream
# engine as a pinned private static library. This avoids both the OpenMW PPA
# and a runtime collision with Noble's older system MyGUI ABI.
mygui_commit="dae9ac4be5a09e672bec509b1a8552b107c40214"
git init --quiet "$source_root"
git -C "$source_root" remote add origin https://github.com/MyGUI/mygui.git
git -C "$source_root" fetch --quiet --depth 1 origin "$mygui_commit"
git -C "$source_root" checkout --quiet --detach FETCH_HEAD
if [[ "$(git -C "$source_root" rev-parse HEAD)" != "$mygui_commit" ]]; then
    echo "MyGUI source checkout did not match the pinned commit" >&2
    exit 1
fi

cmake -S "$source_root" -B "$build_root" \
    -D CMAKE_BUILD_TYPE=Release \
    -D CMAKE_CXX_FLAGS=-DMYGUI_DONT_USE_OBSOLETE=ON \
    -D CMAKE_INSTALL_PREFIX="$install_prefix" \
    -D CMAKE_POSITION_INDEPENDENT_CODE=ON \
    -D MYGUI_GCC_VISIBILITY=OFF \
    -D MYGUI_STATIC=ON \
    -D MYGUI_DISABLE_PLUGINS=ON \
    -D MYGUI_USE_FREETYPE=ON \
    -D MYGUI_RENDERSYSTEM=1 \
    -D MYGUI_BUILD_DEMOS=OFF \
    -D MYGUI_BUILD_PLUGINS=OFF \
    -D MYGUI_BUILD_TOOLS=OFF \
    -D MYGUI_BUILD_UNITTESTS=OFF \
    -D MYGUI_BUILD_TEST_APP=OFF \
    -D MYGUI_BUILD_WRAPPER=OFF \
    -D MYGUI_BUILD_DOCS=OFF
cmake --build "$build_root" --parallel "$(nproc)"
cmake --install "$build_root"
install -Dm0644 "$source_root/COPYING.MIT" \
    "$install_prefix/share/doc/mygui/COPYING.MIT"

test -f "$install_prefix/include/MYGUI/MyGUI.h"
test -f "$install_prefix/lib/libMyGUIEngineStatic.a"
# These thunks must be externally linkable, not merely local definitions in
# the archive. MyGUI's default hidden visibility otherwise leaves OpenMW's
# derived widget vtables with unresolved references.
mygui_symbols="$(nm --extern-only --defined-only "$install_prefix/lib/libMyGUIEngineStatic.a" | c++filt)"
if ! grep -Fq 'non-virtual thunk to MyGUI::Widget::getLayerItemByPoint' <<<"$mygui_symbols"; then
    echo "MyGUI static library does not contain the required widget ABI" >&2
    exit 1
fi
