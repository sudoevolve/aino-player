#!/usr/bin/env bash
set -euo pipefail

main() {
    local aino_base='https://sudoevolve.github.io/aino-player/arch'
    local aino_repo_sha256='132be93a19141d5f0c330a03256002d0114aab75903d314fa550a76d35693043'
    local aino_fingerprint='6BC11916DDE36B879AD6AC8A18681C67CA6020FE'
    local aino_tool
    for aino_tool in pacman pacman-key pacman-conf curl sha256sum mktemp install gpg awk grep uname tee; do
        if ! command -v "$aino_tool" > /dev/null 2>&1; then
            printf 'Required command is missing: %s\n' "$aino_tool" >&2
            return 1
        fi
    done
    if [[ "$(uname -m)" != 'x86_64' ]]; then
        printf 'Aino Player currently provides an x86_64 package only.\n' >&2
        return 1
    fi
    local aino_configured=0
    if pacman-conf --repo-list | grep -Fxq 'aino-player'; then
        # 保留用户的手动配置，避免追加同名仓库或覆盖其它服务器。
        if ! grep -Eq '^[[:space:]]*Include[[:space:]]*=[[:space:]]*/etc/pacman.d/aino-player\.conf[[:space:]]*$' /etc/pacman.conf; then
            printf 'An aino-player repository is already configured manually. Use pacman -Syu aino-player or remove that entry before using this installer.\n' >&2
            return 1
        fi
        aino_configured=1
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
    trap 'rm -f -- "$AINO_INSTALL_TEMP/aino-player.asc" "$AINO_INSTALL_TEMP/aino-player.conf"; rmdir -- "$AINO_INSTALL_TEMP"' EXIT
    curl --proto '=https' --tlsv1.2 -fsSL --retry 3 --connect-timeout 10 --max-time 60 \
        "$aino_base/aino-player.asc" -o "$AINO_INSTALL_TEMP/aino-player.asc"
    curl --proto '=https' --tlsv1.2 -fsSL --retry 3 --connect-timeout 10 --max-time 60 \
        "$aino_base/aino-player.conf" -o "$AINO_INSTALL_TEMP/aino-player.conf"
    printf '%s  %s\n' "$aino_repo_sha256" "$AINO_INSTALL_TEMP/aino-player.conf" | sha256sum --check --status
    gpg --batch --with-colons --show-keys "$AINO_INSTALL_TEMP/aino-player.asc" |
        awk -F: -v expected="$aino_fingerprint" '
            $1 == "pub" { primary++; next_fpr = 1; next }
            $1 == "fpr" && next_fpr { if ($10 == expected) matched++; next_fpr = 0 }
            END { exit !(primary == 1 && matched == 1) }'
    umask "$aino_original_umask"
    if (( EUID != 0 )); then sudo -v; fi
    "${aino_root[@]}" pacman-key --init
    "${aino_root[@]}" pacman-key --add "$AINO_INSTALL_TEMP/aino-player.asc"
    "${aino_root[@]}" pacman-key --lsign-key "$aino_fingerprint"
    "${aino_root[@]}" install -d -m 0755 /etc/pacman.d
    "${aino_root[@]}" install -m 0644 "$AINO_INSTALL_TEMP/aino-player.conf" /etc/pacman.d/aino-player.conf
    if (( ! aino_configured )); then
        printf '\nInclude = /etc/pacman.d/aino-player.conf\n' | "${aino_root[@]}" tee -a /etc/pacman.conf > /dev/null
    fi
    # Arch 必须全量同步升级，不能用 -Sy 后单独装包造成局部升级。
    "${aino_root[@]}" pacman -Syu --noconfirm --needed aino-player
    printf '\nAino Player installed. Start it from the application menu or run aino_player.\n'
}

main "$@"
