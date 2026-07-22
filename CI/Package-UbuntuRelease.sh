#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 6 ]]; then
    echo "usage: $0 BUILD_DIR OUTPUT_DIR SOURCE_COMMIT PACKAGE_VERSION ASSET_NAME MYGUI_PREFIX" >&2
    exit 2
fi

build_dir="$(realpath "$1")"
output_dir="$(realpath -m "$2")"
source_commit="$3"
package_version="$4"
asset_name="$5"
mygui_prefix="$(realpath "$6")"
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
if [[ ! -f "$mygui_prefix/lib/libMyGUIEngineStatic.a" ]]; then
    echo "private MyGUI static library was not found: $mygui_prefix" >&2
    exit 2
fi

mkdir -p "$output_dir"
work_root="$(mktemp -d "${RUNNER_TEMP:-/tmp}/fetcher-ubuntu-package.XXXXXX")"
package_root="$work_root/root"
verify_root="$work_root/verify"
report_path="$output_dir/${asset_name%.deb}-report.txt"
trap 'rm -rf "$work_root"' EXIT

mkdir -p "$package_root" "$verify_root"
DESTDIR="$package_root" cmake --install "$build_dir" --prefix /usr

doc_dir="$package_root/usr/share/doc/fetcher-simulator"
mkdir -p "$doc_dir"
install -m 0644 "$source_root/LICENSE" "$doc_dir/LICENSE"
install -m 0644 "$source_root/README.md" "$doc_dir/README.md"
install -m 0644 "$source_root/AUTHORS.md" "$doc_dir/AUTHORS.md"
install -m 0644 "$mygui_prefix/share/doc/mygui/COPYING.MIT" "$doc_dir/MyGUI-COPYING.MIT"
cat > "$doc_dir/FETCHER-UBUNTU.txt" <<EOF
Fetcher Simulator Ubuntu 24.04 amd64 build
Source commit: $source_commit

This package contains the Fetcher multiplayer OpenMW client, launcher, and
dedicated server. It does not include Morrowind data files or Fetcher gameplay
mods.

Launch the client with openmw-launcher or openmw. Launch the dedicated server
with openmw-server. Server Lua files are installed under
/usr/share/games/openmw/server-scripts.
EOF

# GameNetworkingSockets participates in the top-level CMake install and emits
# development headers and a large static archive. The Ubuntu package consumes
# its shared runtime library only; do not turn this client/server installer into
# an SDK package.
rm -rf "$package_root/usr/include"
find "$package_root/usr/lib" -type f -name '*.a' -delete
find "$package_root/usr/lib" -type d \( -name cmake -o -name pkgconfig \) -prune -exec rm -rf {} +

elf_files=()
while IFS= read -r -d '' candidate; do
    if file --brief "$candidate" | grep -q '^ELF '; then
        elf_files+=("$candidate")
    fi
done < <(find "$package_root/usr" -type f -print0)
if [[ ${#elf_files[@]} -eq 0 ]]; then
    echo "no ELF files were installed" >&2
    exit 1
fi

{
    echo "Fetcher Simulator Ubuntu package report"
    echo "Source commit: $source_commit"
    echo
    echo "ELF files before stripping:"
    for elf_file in "${elf_files[@]}"; do
        printf '%12d  %s  %s\n' \
            "$(stat --format='%s' "$elf_file")" \
            "${elf_file#"$package_root"}" \
            "$(file --brief "$elf_file")"
    done
} > "$report_path"

# RelWithDebInfo is useful while compiling, but the normal installer must not
# carry hundreds of MiB of embedded debug information. Only files positively
# identified as ELF are stripped; data, scripts, and archives are untouched.
for elf_file in "${elf_files[@]}"; do
    strip --strip-unneeded "$elf_file"
done

shlib_args=()
for elf_file in "${elf_files[@]}"; do
    elf_description="$(file --brief "$elf_file")"
    if grep -q 'dynamically linked' <<<"$elf_description"; then
        shlib_args+=("-e$elf_file")
    fi
    if grep -q 'not stripped' <<<"$elf_description"; then
        echo "packaged ELF file still contains embedded debug symbols: $elf_file" >&2
        exit 1
    fi

    dynamic_section="$(readelf --dynamic "$elf_file" 2>/dev/null || true)"
    if grep -Eq '\((RPATH|RUNPATH)\)' <<<"$dynamic_section"; then
        printf '%s\n' "$dynamic_section" | grep -E '\((RPATH|RUNPATH)\)' >> "$report_path"
        if grep -Fq "$build_dir" <<<"$dynamic_section" ||
            grep -Fq "$source_root" <<<"$dynamic_section" ||
            grep -Fq 'runner/work' <<<"$dynamic_section"; then
            echo "packaged ELF file contains a build-directory RPATH/RUNPATH: $elf_file" >&2
            exit 1
        fi
    fi
done
if [[ ${#shlib_args[@]} -eq 0 ]]; then
    echo "no dynamically linked ELF files were installed" >&2
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
if grep -Eqi 'libmygui|librecast' <<<"$dependencies"; then
    echo "package unexpectedly depends on an external MyGUI or Recast runtime: $dependencies" >&2
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

if [[ "$(dpkg-deb --field "$archive_path" Architecture)" != "amd64" ]]; then
    echo "package architecture is not amd64" >&2
    exit 1
fi

dpkg-deb --info "$archive_path"
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

verified_elf_files=()
while IFS= read -r -d '' candidate; do
    if file --brief "$candidate" | grep -q '^ELF '; then
        verified_elf_files+=("$candidate")
    fi
done < <(find "$verify_root/usr" -type f -print0)

for elf_file in "${verified_elf_files[@]}"; do
    if ! file --brief "$elf_file" | grep -q 'x86-64'; then
        echo "packaged ELF file is not amd64: $elf_file" >&2
        exit 1
    fi
    if file --brief "$elf_file" | grep -q 'not stripped'; then
        echo "packaged ELF file is not stripped: $elf_file" >&2
        exit 1
    fi
    if file --brief "$elf_file" | grep -q 'dynamically linked'; then
        ldd_output="$(ldd "$elf_file")"
        if grep -q 'not found' <<<"$ldd_output"; then
            echo "packaged ELF file has unresolved shared libraries: $elf_file" >&2
            printf '%s\n' "$ldd_output" >&2
            exit 1
        fi
    fi
done

if readelf --dynamic "$verify_root/usr/bin/openmw" | grep -Fq 'libMyGUIEngine'; then
    echo "openmw unexpectedly depends on an external MyGUI runtime" >&2
    exit 1
fi

"$verify_root/usr/bin/openmw" --version
"$verify_root/usr/bin/openmw-server" --help >/dev/null

{
    echo
    echo "Package:"
    echo "Compressed-Size: $(stat --format='%s' "$archive_path") bytes"
    echo "Installed-Size: $installed_size KiB"
    echo "Depends: $dependencies"
    echo
    echo "Main executables after stripping:"
    for executable in openmw openmw-launcher openmw-server; do
        executable_path="$verify_root/usr/bin/$executable"
        printf '%12d  /usr/bin/%s  %s\n' \
            "$(stat --format='%s' "$executable_path")" \
            "$executable" \
            "$(file --brief "$executable_path")"
    done
    echo
    echo "Largest 20 packaged files:"
    find "$package_root/usr" -type f -printf '%s\t%p\n' |
        sort --numeric-sort --reverse |
        sed -n '1,20p' |
        sed "s#${package_root}##"
    echo
    sha256sum "$archive_path"
} >> "$report_path"

cat "$report_path"
