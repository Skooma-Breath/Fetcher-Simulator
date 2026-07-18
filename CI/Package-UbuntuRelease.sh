#!/usr/bin/env bash

set -euo pipefail

if [[ $# -lt 4 || $# -gt 5 ]]; then
    echo "usage: $0 BUILD_DIR OUTPUT_DIR SOURCE_COMMIT PACKAGE_VERSION ASSET_NAME" >&2
    exit 2
fi

build_dir="$(realpath "$1")"
output_dir="$(realpath -m "$2")"
source_commit="$3"
package_version="$4"
asset_name="${5:-fetcher-simulator-ubuntu-24.04-amd64.deb}"
source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ ! "$source_commit" =~ ^[0-9a-f]{40}$ ]]; then
    echo "invalid source commit: $source_commit" >&2
    exit 2
fi
if [[ ! "$package_version" =~ ^[0-9][0-9A-Za-z.+~-]*$ ]]; then
    echo "invalid Debian package version: $package_version" >&2
    exit 2
fi
if [[ "$asset_name" != *.deb || "$asset_name" == */* ]]; then
    echo "asset name must be a plain .deb filename: $asset_name" >&2
    exit 2
fi
if [[ ! -f "$build_dir/cmake_install.cmake" ]]; then
    echo "configured build directory was not found: $build_dir" >&2
    exit 2
fi

mkdir -p "$output_dir"
work_root="$(mktemp -d "${RUNNER_TEMP:-/tmp}/fetcher-ubuntu-package.XXXXXX")"
package_root="$work_root/root"
verify_root="$work_root/verify"
trap 'rm -rf "$work_root"' EXIT

mkdir -p "$package_root" "$verify_root"
DESTDIR="$package_root" cmake --install "$build_dir" --prefix /usr

doc_dir="$package_root/usr/share/doc/fetcher-simulator"
mkdir -p "$doc_dir"
install -m 0644 "$source_root/LICENSE" "$doc_dir/LICENSE"
install -m 0644 "$source_root/README.md" "$doc_dir/README.md"
install -m 0644 "$source_root/AUTHORS.md" "$doc_dir/AUTHORS.md"
cat > "$doc_dir/FETCHER-UBUNTU.txt" <<EOF
Fetcher Simulator Ubuntu 24.04 amd64 build
Source commit: $source_commit

This package contains the Fetcher multiplayer OpenMW client and dedicated
server. It does not include Morrowind data files or Fetcher gameplay mods.

Launch the client with openmw-launcher or openmw. Launch the dedicated server
with openmw-server. Server Lua files are installed under
/usr/share/games/openmw/server-scripts.
EOF

mapfile -d '' elf_files < <(find "$package_root/usr/bin" -type f -print0)
shlib_args=()
for binary in "${elf_files[@]}"; do
    if file "$binary" | grep -q 'ELF .* dynamically linked'; then
        shlib_args+=("-e$binary")
    fi
done
if [[ ${#shlib_args[@]} -eq 0 ]]; then
    echo "no dynamically linked ELF binaries were installed" >&2
    exit 1
fi

mkdir -p "$work_root/debian"
cat > "$work_root/debian/control" <<'EOF'
Source: fetcher-simulator
Section: games
Priority: optional
Maintainer: Skooma Breath
Standards-Version: 4.7.0

Package: fetcher-simulator
Architecture: amd64
Description: Fetcher Simulator multiplayer OpenMW client and server
EOF

pushd "$work_root" >/dev/null
shlib_output="$(dpkg-shlibdeps --ignore-missing-info -O "${shlib_args[@]}")"
popd >/dev/null
dependencies="${shlib_output#shlibs:Depends=}"
if [[ -z "$dependencies" || "$dependencies" == "$shlib_output" ]]; then
    echo "dpkg-shlibdeps did not produce package dependencies" >&2
    exit 1
fi

control_dir="$package_root/DEBIAN"
mkdir -p "$control_dir"
installed_size="$(du -sk "$package_root/usr" | cut -f1)"
cat > "$control_dir/control" <<EOF
Package: fetcher-simulator
Version: $package_version
Section: games
Priority: optional
Architecture: amd64
Maintainer: Skooma Breath
Depends: $dependencies
Installed-Size: $installed_size
Homepage: https://github.com/Skooma-Breath/Fetcher-Simulator
Description: Fetcher Simulator multiplayer OpenMW client and server
 Fetcher's OpenMW fork with multiplayer client support and the dedicated
 openmw-server runtime. Game data and tester mods are not included.
EOF

archive_path="$output_dir/$asset_name"
rm -f "$archive_path"
dpkg-deb --build --root-owner-group "$package_root" "$archive_path"
dpkg-deb --info "$archive_path"
dpkg-deb --contents "$archive_path"
dpkg-deb --extract "$archive_path" "$verify_root"

for required in \
    usr/bin/openmw \
    usr/bin/openmw-launcher \
    usr/bin/openmw-server \
    usr/share/games/openmw/server-scripts/core.lua \
    usr/share/games/openmw/lua_libs/util.lua; do
    if [[ ! -f "$verify_root/$required" ]]; then
        echo "packaged file is missing: $required" >&2
        exit 1
    fi
done

for binary in openmw openmw-server; do
    if ldd "$verify_root/usr/bin/$binary" | grep -q 'not found'; then
        echo "packaged $binary has unresolved shared libraries" >&2
        ldd "$verify_root/usr/bin/$binary" >&2
        exit 1
    fi
done

"$verify_root/usr/bin/openmw" --version
"$verify_root/usr/bin/openmw-server" --help >/dev/null
sha256sum "$archive_path"

