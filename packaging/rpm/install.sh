#!/usr/bin/env bash
set -euo pipefail

main() {
    local aino_base='https://sudoevolve.github.io/aino-player/rpm'
    local aino_repo_sha256='c5c2b1dcb4fd4889cc65ae8bf0e76d24b9cad31cd7940397cc522cf8631f9992'
    local aino_fingerprint='6BC11916DDE36B879AD6AC8A18681C67CA6020FE'
    local aino_tool
    for aino_tool in curl sha256sum mktemp install gpg rpm awk uname; do
        if ! command -v "$aino_tool" > /dev/null 2>&1; then
            printf 'Required command is missing: %s\n' "$aino_tool" >&2
            return 1
        fi
    done
    if [[ "$(uname -m)" != 'x86_64' ]]; then
        printf 'Aino Player currently provides an x86_64 package only.\n' >&2
        return 1
    fi
    local aino_manager aino_repos
    if command -v dnf > /dev/null 2>&1; then
        aino_manager=dnf
        aino_repos='/etc/yum.repos.d'
    else
        printf 'This installer requires dnf (Fedora 44 or newer).\n' >&2
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
    trap 'rm -f -- "$AINO_INSTALL_TEMP/aino-player.asc" "$AINO_INSTALL_TEMP/aino-player.repo"; rmdir -- "$AINO_INSTALL_TEMP"' EXIT
    curl --proto '=https' --tlsv1.2 -fsSL --retry 3 --connect-timeout 10 --max-time 60 \
        "$aino_base/aino-player.asc" -o "$AINO_INSTALL_TEMP/aino-player.asc"
    curl --proto '=https' --tlsv1.2 -fsSL --retry 3 --connect-timeout 10 --max-time 60 \
        "$aino_base/aino-player.repo" -o "$AINO_INSTALL_TEMP/aino-player.repo"
    printf '%s  %s\n' "$aino_repo_sha256" "$AINO_INSTALL_TEMP/aino-player.repo" | sha256sum --check --status
    # 公钥每次部署重新导出，固定比对指纹而不是文件字节。
    gpg --batch --with-colons --show-keys "$AINO_INSTALL_TEMP/aino-player.asc" |
        awk -F: -v expected="$aino_fingerprint" '
            $1 == "pub" { primary++; next_fpr = 1; next }
            $1 == "fpr" && next_fpr { if ($10 == expected) matched++; next_fpr = 0 }
            END { exit !(primary == 1 && matched == 1) }'

    umask "$aino_original_umask"

    # Downloads and pinned digests are checked before requesting administrator access.
    if (( EUID != 0 )); then
        sudo -v
    fi
    "${aino_root[@]}" install -d -m 0755 "$aino_repos"
    "${aino_root[@]}" install -m 0644 "$AINO_INSTALL_TEMP/aino-player.repo" "$aino_repos/aino-player.repo"
    "${aino_root[@]}" rpm --quiet --import "$AINO_INSTALL_TEMP/aino-player.asc"
    "${aino_root[@]}" "$aino_manager" install -y aino-player
    printf '\nAino Player installed. Start it from the application menu or run aino_player.\n'
}

main "$@"
