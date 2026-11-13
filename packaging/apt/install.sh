#!/usr/bin/env bash
set -euo pipefail

main() {
    local aino_base='https://sudoevolve.github.io/aino-player/apt'
    local aino_key_sha256='bb5d2dacb6cb19b5fe59d21664697020354747006a1d3a367af48c914c901073'
    local aino_source_sha256='897588024d9951972d10cbf9cf0f15880c85de040854aa4c5880e5a1c5efd118'
    local aino_tool
    for aino_tool in apt-get dpkg curl sha256sum mktemp install; do
        if ! command -v "$aino_tool" > /dev/null 2>&1; then
            printf 'Required command is missing: %s\n' "$aino_tool" >&2
            return 1
        fi
    done
    if [[ "$(dpkg --print-architecture)" != 'amd64' ]]; then
        printf 'Aino Player currently provides an amd64 package only.\n' >&2
        return 1
    fi

    local -a aino_root=()
    if (( EUID != 0 )); then
        if ! command -v sudo > /dev/null 2>&1; then
            printf 'Run this installer as root or install sudo first.\n' >&2
            return 1
        fi
        aino_root=(sudo)
    fi

    local aino_original_umask
    aino_original_umask=$(umask)
    umask 077
    AINO_INSTALL_TEMP=$(mktemp -d)
    trap 'rm -f -- "$AINO_INSTALL_TEMP/key.gpg" "$AINO_INSTALL_TEMP/aino-player.sources"; rmdir -- "$AINO_INSTALL_TEMP"' EXIT
    curl --proto '=https' --tlsv1.2 -fsSL --retry 3 --connect-timeout 10 --max-time 60 \
        "$aino_base/aino-player-archive-keyring.gpg" -o "$AINO_INSTALL_TEMP/key.gpg"
    curl --proto '=https' --tlsv1.2 -fsSL --retry 3 --connect-timeout 10 --max-time 60 \
        "$aino_base/aino-player.sources" -o "$AINO_INSTALL_TEMP/aino-player.sources"
    printf '%s  %s\n' "$aino_key_sha256" "$AINO_INSTALL_TEMP/key.gpg" \
        "$aino_source_sha256" "$AINO_INSTALL_TEMP/aino-player.sources" | sha256sum --check --status

    umask "$aino_original_umask"

    # Downloads and pinned digests are checked before requesting administrator access.
    if (( EUID != 0 )); then
        sudo -v
    fi
    "${aino_root[@]}" install -d -m 0755 /etc/apt/keyrings /etc/apt/sources.list.d
    "${aino_root[@]}" install -m 0644 "$AINO_INSTALL_TEMP/key.gpg" /etc/apt/keyrings/aino-player-archive-keyring.gpg
    "${aino_root[@]}" install -m 0644 "$AINO_INSTALL_TEMP/aino-player.sources" /etc/apt/sources.list.d/aino-player.sources
    "${aino_root[@]}" apt-get update
    "${aino_root[@]}" apt-get install -y aino-player
    printf '\nAino Player installed. Start it from the application menu or run aino_player.\n'
}

main "$@"
