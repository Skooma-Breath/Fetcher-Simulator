#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 PACKAGE.deb" >&2
    exit 2
fi

package_path="$(realpath "$1")"
if [[ ! -f "$package_path" || "$package_path" != *.deb ]]; then
    echo "Ubuntu package was not found: $package_path" >&2
    exit 2
fi

docker run --rm --platform linux/amd64 \
    --volume "$package_path:/tmp/fetcher-simulator.deb:ro" \
    ubuntu:24.04 \
    bash -euxo pipefail -c '
        export DEBIAN_FRONTEND=noninteractive
        if grep -RqsE "openmw/(openmw|openmw-daily)|openmw/staging" /etc/apt 2>/dev/null; then
            echo "clean test image unexpectedly contains an OpenMW PPA" >&2
            exit 1
        fi

        apt-get update
        apt-get install -y /tmp/fetcher-simulator.deb
        dpkg-query -W -f="\${Package} \${Status} \${Architecture} \${Version}\n" fetcher-simulator
        if dpkg-query -W -f="\${binary:Package}\n" | grep -Eq "^(libmygui|librecast)"; then
            echo "clean install unexpectedly pulled an external MyGUI or Recast runtime" >&2
            exit 1
        fi

        [[ "$(dpkg-query -W -f="\${Architecture}" fetcher-simulator)" == "amd64" ]]
        for executable in openmw openmw-launcher openmw-server; do
            installed_path="$(command -v "$executable")"
            [[ -n "$installed_path" && -x "$installed_path" ]]
            echo "$executable: $installed_path"
        done

        mapfile -t elf_files < <(
            while IFS= read -r installed_file; do
                if [[ -f "$installed_file" ]] &&
                    [[ "$(od -An -tx1 -N4 "$installed_file")" == " 7f 45 4c 46" ]]; then
                    printf "%s\n" "$installed_file"
                fi
            done < <(dpkg-query -L fetcher-simulator)
        )
        [[ ${#elf_files[@]} -gt 0 ]]
        for elf_file in "${elf_files[@]}"; do
            ldd_output="$(ldd "$elf_file")"
            printf "%s\n" "$ldd_output"
            if grep -q "not found" <<<"$ldd_output"; then
                echo "unresolved shared library in $elf_file" >&2
                exit 1
            fi
        done

        openmw --version
        openmw-server --help >/dev/null
        test -f /usr/share/games/openmw/server-scripts/core.lua
        test -f /usr/share/games/openmw/lua_libs/util.lua
    '
